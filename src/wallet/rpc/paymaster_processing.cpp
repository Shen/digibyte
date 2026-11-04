// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Provider request, submit, result, and recovery message processing. */

#include <wallet/rpc/paymaster.h>
#include <wallet/rpc/paymaster_internal.h>

#include <base58.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <digidollar/validation.h>
#include <hash.h>
#include <index/txindex.h>
#include <interfaces/chain.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <net.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/context.h>
#include <node/transaction.h>
#include <oracle/bundle_manager.h>
#include <paymaster/client.h>
#include <paymaster/directory.h>
#include <paymaster/manager.h>
#include <paymaster/protocol.h>
#include <paymaster/reservation.h>
#include <paymaster/validation.h>
#include <random.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <streams.h>
#include <version.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/walletdb.h>

#include <univalue.h>
#include <util/overflow.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>

namespace wallet {
using namespace DigiDollar::Paymaster;
using namespace paymaster_rpc::internal;

RPCHelpMan processpaymasterresult()
{
    return RPCHelpMan{
        "processpaymasterresult",
        "Validate and persist at most one PMRESULT for an existing client session, or reconcile "
        "its exact locally observed payment after 240 confirmations without a provider result.\n",
        {{"request_id", RPCArg::Type::STR, RPCArg::Optional::NO, "Canonical request UUID"}},
        RPCResult{RPCResult::Type::OBJ, "", "Authoritative result processing state", {
                                                                                         {RPCResult::Type::BOOL, "processed", "Whether a matching result or newly reconciled confirmed payment was available"},
                                                                                         {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                         {RPCResult::Type::STR_HEX, "session_id", "Persistent session"},
                                                                                         {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                         {RPCResult::Type::STR_HEX, "offer_id", "Exact authorized offer"},
                                                                                         {RPCResult::Type::STR_HEX, "policy_hash", "Exact authorized provider policy"},
                                                                                         {RPCResult::Type::STR, "funding_model", "Exact authorized funding model"},
                                                                                         {RPCResult::Type::NUM, "payment_cents", "Exact recipient amount"},
                                                                                         {RPCResult::Type::NUM, "service_fee_cents", "Exact provider service fee"},
                                                                                         {RPCResult::Type::NUM, "user_total_cents", "Exact maximum wallet outflow"},
                                                                                         {RPCResult::Type::STR_HEX, "authorization_commitment", "Accepted client authorization manifest"},
                                                                                         {RPCResult::Type::BOOL, "authorization_accepted", "Whether the exact manifest remains accepted"},
                                                                                         {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                         {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                         {RPCResult::Type::STR, "result_status", /*optional=*/true, "Signed provider status"},
                                                                                         {RPCResult::Type::NUM, "result_sequence", /*optional=*/true, "Monotonic sequence"},
                                                                                         {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Known final transaction"},
                                                                                     }},
        RPCExamples{HelpExampleCli("processpaymasterresult", "\"550e8400-e29b-41d4-a716-446655440000\"")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            using namespace DigiDollar::Paymaster;
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            return ProcessPaymasterResult(context, *wallet, request.params[0].get_str());
        }),
    };
}

RPCHelpMan processpaymastersubmits()
{
    return RPCHelpMan{
        "processpaymastersubmits",
        "Process at most one addressed PMSUBMIT through the durable provider commit path.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider submit processing result", {
                                                                                     {RPCResult::Type::BOOL, "processed", "Whether a submit was available"},
                                                                                     {RPCResult::Type::BOOL, "queued", "Whether PMRESULT was queued"},
                                                                                     {RPCResult::Type::BOOL, "rejected", /*optional=*/true, "Whether this stale or mismatched submission was discarded"},
                                                                                     {RPCResult::Type::STR, "rejection_reason", /*optional=*/true, "Reason for discarding only this submission"},
                                                                                     {RPCResult::Type::STR, "message_type", /*optional=*/true, "submit or recovery_submit"},
                                                                                     {RPCResult::Type::STR, "request_id", /*optional=*/true, "Canonical request UUID"},
                                                                                     {RPCResult::Type::STR_HEX, "session_id", /*optional=*/true, "Client session"},
                                                                                     {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider identity"},
                                                                                     {RPCResult::Type::STR_HEX, "commit_key", /*optional=*/true, "Durable commit key"},
                                                                                     {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Final transaction"},
                                                                                     {RPCResult::Type::STR, "result_status", /*optional=*/true, "Signed provider result status"},
                                                                                     {RPCResult::Type::STR, "attempt_state", /*optional=*/true, "Authoritative provider attempt state"},
                                                                                     {RPCResult::Type::STR, "rejection_code", /*optional=*/true, "Stable local reason for a rejected stale submit"},
                                                                                     {RPCResult::Type::BOOL, "broadcast", /*optional=*/true, "Whether an alternative-recovery transaction was broadcast"},
                                                                                     {RPCResult::Type::STR, "broadcast_error", /*optional=*/true, "Stable alternative-recovery broadcast error"},
                                                                                     {RPCResult::Type::OBJ, "recovery", /*optional=*/true, "Persistent alternative-recovery authority and state", {
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "recovery_id", "Stable recovery identifier"},
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "recovery_provider_id", "Distinct recovery provider"},
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "offer_id", "Exact USER_PAID offer"},
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "policy_hash", "Exact provider policy"},
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "original_commit_key", "Ambiguous original provider commit"},
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "original_template_commitment", "Ambiguous original transaction template"},
                                                                                                                                                                                                      {RPCResult::Type::STR, "privacy_profile", "Inherited standard or high privacy profile"},
                                                                                                                                                                                                      {RPCResult::Type::STR, "phase", "capacity_pending, request_ready, response_validated, user_signed, or final_committed"},
                                                                                                                                                                                                      {RPCResult::Type::BOOL, "expired", "Whether a safely expirable unsigned recovery expired"},
                                                                                                                                                                                                      {RPCResult::Type::ARR, "user_dd_inputs", "Canonical original user inputs", {{RPCResult::Type::OBJ, "", "User DD input", {
                                                                                                                                                                                                                                                                                                                                  {RPCResult::Type::STR_HEX, "txid", "Creating transaction"},
                                                                                                                                                                                                                                                                                                                                  {RPCResult::Type::NUM, "vout", "Output index"},
                                                                                                                                                                                                                                                                                                                              }}}},
                                                                                                                                                                                                      {RPCResult::Type::ARR, "wallet_returns", "Canonical wallet-owned recovery outputs", {{RPCResult::Type::OBJ, "", "Wallet return", {
                                                                                                                                                                                                                                                                                                                                           {RPCResult::Type::STR_HEX, "script_pub_key", "Exact return script"},
                                                                                                                                                                                                                                                                                                                                           {RPCResult::Type::STR, "address", /*optional=*/true, "Display address when decodable"},
                                                                                                                                                                                                                                                                                                                                           {RPCResult::Type::NUM, "amount_cents", "Exact DD amount"},
                                                                                                                                                                                                                                                                                                                                       }}}},
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "capacity_snapshot_id", /*optional=*/true, "Validated Capacity-v5 snapshot"},
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "capacity_resource_commitment", /*optional=*/true, "Exact capacity resources"},
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "authorization_commitment", /*optional=*/true, "Wallet-local exact recovery authorization"},
                                                                                                                                                                                                      {RPCResult::Type::BOOL, "authorization_accepted", /*optional=*/true, "Whether that exact commitment was accepted"},
                                                                                                                                                                                                      {RPCResult::Type::NUM_TIME, "authorization_accepted_at", /*optional=*/true, "Durable acceptance time"},
                                                                                                                                                                                                      {RPCResult::Type::NUM, "maximum_service_fee_cents", "Effective local fee ceiling"},
                                                                                                                                                                                                      {RPCResult::Type::NUM, "service_fee_cents", "Exact recovery service fee"},
                                                                                                                                                                                                      {RPCResult::Type::NUM, "network_fee_satoshis", /*optional=*/true, "Exact provider-paid network fee"},
                                                                                                                                                                                                      {RPCResult::Type::NUM_TIME, "expires_at", "Recovery authorization expiry"},
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "raw_transaction", /*optional=*/true, "Exact final recovery transaction"},
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Final recovery txid"},
                                                                                                                                                                                                      {RPCResult::Type::STR_HEX, "wtxid", /*optional=*/true, "Final recovery wtxid"},
                                                                                                                                                                                                  }},
                                                                                     {RPCResult::Type::OBJ, "commit", /*optional=*/true, "Durable provider commit and broadcast result", {
                                                                                                                                                                                             {RPCResult::Type::STR, "psbt", /*optional=*/true, "Canonical fully signed PSBT when produced by this call"},
                                                                                                                                                                                             {RPCResult::Type::STR_HEX, "hex", "Exact durably committed final transaction"},
                                                                                                                                                                                             {RPCResult::Type::STR_HEX, "txid", "Final transaction identifier"},
                                                                                                                                                                                             {RPCResult::Type::BOOL, "broadcast", "Whether the committed transaction is in a local pool or already confirmed"},
                                                                                                                                                                                             {RPCResult::Type::STR, "broadcast_error", /*optional=*/true, "Stable recovery error when the durable commit could not yet be broadcast"},
                                                                                                                                                                                             {RPCResult::Type::STR, "request_id", "Canonical request UUID"},
                                                                                                                                                                                             {RPCResult::Type::STR_HEX, "session_id", "Persistent session identifier"},
                                                                                                                                                                                             {RPCResult::Type::STR_HEX, "provider_id", "Provider identity"},
                                                                                                                                                                                             {RPCResult::Type::STR_HEX, "attempt_id", "Persistent provider attempt"},
                                                                                                                                                                                             {RPCResult::Type::STR_HEX, "quote_id", "Bound quote identifier"},
                                                                                                                                                                                             {RPCResult::Type::STR_HEX, "unsigned_txid", "Witness-free unsigned transaction identifier"},
                                                                                                                                                                                             {RPCResult::Type::STR_HEX, "template_commitment", "Bound unsigned transaction commitment"},
                                                                                                                                                                                             {RPCResult::Type::STR, "session_state", "Authoritative session state"},
                                                                                                                                                                                             {RPCResult::Type::STR, "attempt_state", "Authoritative attempt state"},
                                                                                                                                                                                             {RPCResult::Type::STR, "result_status", "Signed provider result status"},
                                                                                                                                                                                             {RPCResult::Type::NUM, "result_sequence", "Monotonic result sequence"},
                                                                                                                                                                                             {RPCResult::Type::NUM_TIME, "expires_at", "Exact retry deadline"},
                                                                                                                                                                                         }},
                                                                                 }},
        RPCExamples{HelpExampleCli("processpaymastersubmits", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            return ProcessPaymasterSubmits(context, *wallet, ExecutionMode::MANUAL);
        }),
    };
}

RPCHelpMan processpaymasterrequests()
{
    return RPCHelpMan{
        "processpaymasterrequests",
        "Process at most one addressed PMCAPREQ or PMQUOTEREQ for this running provider wallet. "
        "Capacity is proven before any payment intent is accepted. New proofs and quotes require "
        "an unlocked wallet; exact durable retries do not.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Provider quote processing result", {
                                                                                    {RPCResult::Type::BOOL, "processed", "Whether a request was available"},
                                                                                    {RPCResult::Type::BOOL, "queued", "Whether the signed response was queued"},
                                                                                    {RPCResult::Type::BOOL, "deferred", /*optional=*/true, "Whether this request is waiting within its original expiry for admission limits to recover"},
                                                                                    {RPCResult::Type::STR, "deferral_reason", /*optional=*/true, "Request admission limit; other eligible requests and submissions may continue"},
                                                                                    {RPCResult::Type::BOOL, "rejected", /*optional=*/true, "Whether a permanently invalid inbound continuation was discarded"},
                                                                                    {RPCResult::Type::STR, "rejection_reason", /*optional=*/true, "Stable reason for discarding a permanently invalid continuation"},
                                                                                    {RPCResult::Type::STR, "message_type", /*optional=*/true, "capacity, recovery_request, or quote"},
                                                                                    {RPCResult::Type::STR_HEX, "client_nonce", /*optional=*/true, "Capacity-bound client nonce"},
                                                                                    {RPCResult::Type::STR_HEX, "capacity_snapshot_id", /*optional=*/true, "Signed capacity snapshot"},
                                                                                    {RPCResult::Type::STR, "request_id", /*optional=*/true, "Canonical request UUID"},
                                                                                    {RPCResult::Type::STR_HEX, "session_id", /*optional=*/true, "Client session"},
                                                                                    {RPCResult::Type::STR_HEX, "provider_id", /*optional=*/true, "Provider identity"},
                                                                                    {RPCResult::Type::STR_HEX, "attempt_id", /*optional=*/true, "Durable provider attempt"},
                                                                                    {RPCResult::Type::STR_HEX, "recovery_id", /*optional=*/true, "Stable alternative-recovery identifier"},
                                                                                    {RPCResult::Type::STR_HEX, "quote_id", /*optional=*/true, "Signed quote"},
                                                                                    {RPCResult::Type::STR_HEX, "template_commitment", /*optional=*/true, "Exact template"},
                                                                                }},
        RPCExamples{HelpExampleCli("processpaymasterrequests", "")},
        PaymasterRpcAdapter([](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            WalletContext& context = EnsureWalletContext(request.context);
            std::shared_ptr<CWallet> wallet = GetWalletForJSONRPCRequest(request);
            if (!wallet) return UniValue::VNULL;
            return ProcessPaymasterRequests(context, *wallet, ExecutionMode::MANUAL);
        }),
    };
}

} // namespace wallet
