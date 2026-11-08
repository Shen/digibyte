// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <hash.h>
#include <key.h>
#include <paymaster/directory.h>
#include <paymaster/finance.h>
#include <paymaster/psbt.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <script/interpreter.h>
#include <script/standard.h>
#include <univalue.h>
#include <util/time.h>
#include <wallet/context.h>
#include <wallet/paymasterfinance.h>
#include <wallet/paymasterfinancecache.h>
#include <wallet/paymastercheckpoint.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterapplication_internal.h>
#include <wallet/paymasterstore.h>
#include <wallet/paymasterstore_internal.h>
#include <wallet/rpc/paymaster.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <chrono>
#include <optional>

namespace wallet {
using namespace DigiDollar::Paymaster;
namespace {
struct FinanceReads {
    size_t ranges{0};
    size_t prefixes{0};
    size_t rows{0};
    std::optional<SerializeData> unreadable_key;
    size_t read_failures{0};
    size_t failed_existence_checks{0};
};

class FinanceCursor final : public DatabaseCursor {
    std::unique_ptr<DatabaseCursor> m_cursor;
    FinanceReads& m_reads;
public:
    FinanceCursor(std::unique_ptr<DatabaseCursor> cursor, FinanceReads& reads)
        : m_cursor{std::move(cursor)}, m_reads{reads} {}
    Status Next(DataStream& key, DataStream& value) override
    {
        const auto status = m_cursor->Next(key, value);
        if (status == Status::MORE) ++m_reads.rows;
        return status;
    }
};

class FinanceBatch final : public MockableBatch {
    FinanceReads& m_reads;
    MockableDatabase& m_database;

    DatabaseReadStatus ReadKey(DataStream&& key, DataStream& value) override
    {
        const SerializeData encoded{key.begin(), key.end()};
        if (m_reads.unreadable_key == encoded) {
            ++m_reads.read_failures;
            return DatabaseReadStatus::READ_ERROR;
        }
        if (!m_database.m_pass) return DatabaseReadStatus::READ_ERROR;
        const auto found = m_database.m_records.find(encoded);
        if (found == m_database.m_records.end()) return DatabaseReadStatus::NOT_FOUND;
        value.clear();
        value.write(found->second);
        return DatabaseReadStatus::FOUND;
    }

    bool HasKey(DataStream&& key) override
    {
        const SerializeData encoded{key.begin(), key.end()};
        if (m_reads.unreadable_key == encoded) {
            ++m_reads.failed_existence_checks;
            return false;
        }
        return m_database.m_pass && m_database.m_records.count(encoded) != 0;
    }
public:
    FinanceBatch(MockableDatabase& db, FinanceReads& reads)
        : MockableBatch(db.m_records, db.m_pass, &db.m_on_write, &db.m_refuse_write,
                        db.m_txn_pass, &db.m_fail_write_at, &db.m_write_count, &db.m_fail_commit),
          m_reads{reads}, m_database{db} {}
    std::unique_ptr<DatabaseCursor> GetNewPrefixCursor(Span<const std::byte> prefix) override
    {
        ++m_reads.prefixes;
        return std::make_unique<FinanceCursor>(MockableBatch::GetNewPrefixCursor(prefix), m_reads);
    }
    std::unique_ptr<DatabaseCursor> GetNewRangeCursor(Span<const std::byte> begin, Span<const std::byte> end) override
    {
        ++m_reads.ranges;
        return std::make_unique<FinanceCursor>(MockableBatch::GetNewRangeCursor(begin, end), m_reads);
    }
};

class FinanceDatabase final : public MockableDatabase {
public:
    FinanceReads reads;
    std::unique_ptr<DatabaseBatch> MakeBatch(bool = true) override
    {
        return std::make_unique<FinanceBatch>(*this, reads);
    }
};

struct FinanceTestingSetup : WalletTestingSetup {
    FinanceDatabase* database;
    std::unique_ptr<CWallet> provider;
    ProviderIdentityRecord identity;
    std::string error;

    FinanceTestingSetup()
    {
        auto db = std::make_unique<FinanceDatabase>();
        database = db.get();
        provider = std::make_unique<CWallet>(m_node.chain.get(), "incremental-finance", std::move(db));
        BOOST_REQUIRE(provider->LoadWallet() == DBErrors::LOAD_OK);
        LOCK(provider->cs_wallet);
        provider->SetLastBlockProcessed(100, Params().GenesisBlock().GetHash());
        provider->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        provider->SetupDescriptorScriptPubKeyMans();
        BOOST_REQUIRE(CreatePaymasterIdentity(*provider, "Finance", 1000, identity, error));
    }

    ProviderFinanceMetadata Metadata()
    {
        ProviderFinanceMetadata metadata;
        BOOST_REQUIRE(WalletBatch{*database}.ReadPaymasterFinanceMetadata(metadata) == DatabaseReadStatus::FOUND);
        return metadata;
    }

    ProviderFinanceEvent Event(size_t id, const uint256& txid = {})
    {
        ProviderFinanceEvent event;
        event.event_id = uint256S(strprintf("%064x", id + 1));
        event.transaction_id = txid.IsNull() ? event.event_id : txid;
        event.provider_id = identity.provider_id;
        event.genesis_hash = Params().GenesisBlock().GetHash();
        event.kind = ProviderFinanceEventKind::TRANSFER;
        event.funding_model = FundingModel::USER_PAID;
        event.dd_income = DDCents{3};
        event.dgb_cost = DGBSatoshis{7};
        event.created_at = event.updated_at = 1000;
        return event;
    }

    CWalletTx* Transaction()
    {
        LOCK(provider->cs_wallet);
        CMutableTransaction transaction;
        transaction.vin.emplace_back(COutPoint{uint256S("1234"), 0});
        transaction.vout.emplace_back(1000, CScript{} << OP_TRUE);
        return provider->AddToWallet(MakeTransactionRef(transaction), TxStateInactive{});
    }

    ProviderMaintenanceRecord Maintenance(bool journaled)
    {
        LOCK(provider->cs_wallet);
        const auto destination = provider->GetNewDestination(OutputType::BECH32M, "exact maintenance fee");
        BOOST_REQUIRE(destination);
        const CScript script = GetScriptForDestination(*destination);
        CMutableTransaction funding;
        funding.vin.emplace_back(COutPoint{uint256S("f101"), 0});
        funding.vout.emplace_back(MIN_ADMISSION_DGB_SATOSHIS + 7, identity.identity_script);
        const auto parent = MakeTransactionRef(funding);
        BOOST_REQUIRE(provider->AddToWallet(parent, TxStateConfirmed{Params().GenesisBlock().GetHash(), 1, 0}));
        CMutableTransaction maintenance;
        maintenance.vin.emplace_back(COutPoint{parent->GetHash(), 0});
        maintenance.vout.emplace_back(MIN_ADMISSION_DGB_SATOSHIS, script);
        const auto transaction = MakeTransactionRef(maintenance);
        BOOST_REQUIRE(provider->AddToWallet(transaction, TxStateConfirmed{Params().GenesisBlock().GetHash(), 2, 0}));
        ProviderMaintenanceRecord record;
        record.operation_id = uint256S("f102");
        record.plan_id = uint256S("f103");
        record.maximum_fee = DGBSatoshis{100};
        record.created_at = record.updated_at = 1000;
        ProviderMaintenanceOutput output;
        output.script_pub_key = script;
        output.dgb_value = DGBSatoshis{MIN_ADMISSION_DGB_SATOSHIS};
        record.outputs = {output};
        if (journaled) {
            record.transaction_id = transaction->GetHash();
            record.state = ProviderMaintenanceState::CONFIRMED;
            record.actual_fee = record.maximum_fee;
        }
        return record;
    }

    ProviderFinanceEvent EstimatedMaintenanceEvent(const ProviderMaintenanceRecord& record)
    {
        auto event = Event(900);
        event.event_id = record.operation_id;
        event.transaction_id = record.transaction_id;
        event.kind = ProviderFinanceEventKind::LIQUIDITY_REPLENISHMENT;
        event.dd_income = DDCents{0};
        event.dgb_cost = record.maximum_fee;
        event.state = ProviderFinanceEventState::CONFIRMED;
        event.confirmed_at = event.updated_at = 1001;
        return event;
    }

    std::pair<ProviderAttempt, ProviderCommitRecord> Transfer()
    {
        using paymaster_store::internal::CanonicalBytes;
        LOCK(provider->cs_wallet);
        CKey signer, user_key, spending_key;
        ProviderIdentityRecord signing_identity;
        BOOST_REQUIRE(GetPaymasterIdentityKey(*provider, signer, signing_identity, error));
        ProviderPolicy policy;
        policy.funding_models = FUNDING_MODEL_USER_PAID;
        policy.fee_rate_bps = 1000;
        policy.min_payment = DDCents{100};
        policy.max_payment = DDCents{100000};
        policy.quote_ttl = 60;
        policy.maximum_network_fee = DGBSatoshis{500};
        user_key.MakeNewKey(true);
        spending_key.MakeNewKey(true);
        TaprootBuilder user_taproot, provider_taproot;
        user_taproot.Finalize(XOnlyPubKey{user_key.GetPubKey()});
        provider_taproot.Finalize(XOnlyPubKey{spending_key.GetPubKey()});
        const CScript user_script = GetScriptForDestination(user_taproot.GetOutput());
        const CScript provider_script = GetScriptForDestination(provider_taproot.GetOutput());
        CMutableTransaction user_previous, provider_previous;
        user_previous.SetDigiDollarType(::DD_TX_TRANSFER);
        user_previous.vin.emplace_back(COutPoint{uint256S("f301"), 0});
        user_previous.vout.emplace_back(0, user_script);
        user_previous.vout.emplace_back(0, CScript{} << OP_RETURN <<
            std::vector<unsigned char>{'D', 'D'} << CScriptNum(::DD_TX_TRANSFER) << CScriptNum(1100));
        provider_previous.vin.emplace_back(COutPoint{uint256S("f302"), 0});
        provider_previous.vout.emplace_back(2000, provider_script);
        const auto user_parent = MakeTransactionRef(user_previous);
        const auto provider_parent = MakeTransactionRef(provider_previous);
        CMutableTransaction transaction;
        transaction.SetDigiDollarType(::DD_TX_TRANSFER);
        transaction.vin.emplace_back(COutPoint{user_parent->GetHash(), 0});
        transaction.vin.emplace_back(COutPoint{provider_parent->GetHash(), 0});
        transaction.vout.emplace_back(0, user_script);
        transaction.vout.emplace_back(0, provider_script);
        transaction.vout.emplace_back(1500, provider_script);
        transaction.vout.emplace_back(0, CScript{} << OP_RETURN <<
            std::vector<unsigned char>{'D', 'D'} << CScriptNum(::DD_TX_TRANSFER) << CScriptNum(1000) << CScriptNum(100));
        CollaborativePSBTTemplate trusted;
        BOOST_REQUIRE_MESSAGE(CreateCollaborativePSBTTemplate(transaction,
            {{transaction.vin[0].prevout, user_parent, InputRole::USER_DD},
             {transaction.vin[1].prevout, provider_parent, InputRole::PROVIDER_DGB}}, trusted, error), error);
        PaymentIntent intent;
        intent.genesis_hash = Params().GenesisBlock().GetHash();
        intent.provider_id = identity.provider_id;
        intent.request_id = "550e8400-e29b-41d4-a716-446655449005";
        intent.session_id = uint256S("f303");
        intent.client_nonce = uint256S("f304");
        intent.canonical_request_hash = uint256S("f305");
        intent.user_dd_inputs = {transaction.vin[0].prevout};
        intent.recipient_script = intent.user_dd_change_script = user_script;
        intent.recipient_amount = DDCents{1000};
        intent.offer_id = uint256S("f306");
        intent.policy_hash = GetProviderPolicyHash(policy);
        intent.funding_model = FundingModel::USER_PAID;
        intent.expires_at = 2060;
        const uint256 empty_merkle_root;
        UserInputControlProof proof{intent.user_dd_inputs.front(), std::vector<unsigned char>(64)};
        BOOST_REQUIRE(user_key.SignSchnorr(GetUserInputControlHash(intent, proof.outpoint),
            proof.signature, &empty_merkle_root, uint256{}));
        intent.user_input_proofs = {proof};
        BOOST_REQUIRE_MESSAGE(ValidatePaymentIntent(intent, intent.genesis_hash, 2000,
            {XOnlyPubKey{user_taproot.GetOutput()}}, error), error);
        PaymasterQuote quote;
        quote.genesis_hash = intent.genesis_hash;
        quote.provider_id = identity.provider_id;
        quote.quote_id = uint256S("f308");
        quote.intent_hash = GetPaymentIntentHash(intent);
        quote.offer_id = intent.offer_id;
        quote.policy_hash = intent.policy_hash;
        quote.funding_model = intent.funding_model;
        quote.fee_rate_bps = policy.fee_rate_bps;
        quote.service_fee = DDCents{100};
        quote.network_fee = DGBSatoshis{500};
        quote.provider_fee_script = quote.dgb_change_script = provider_script;
        quote.reserved_dgb_inputs = {{transaction.vin[1].prevout, provider_previous, DGBSatoshis{2000}}};
        quote.created_at = 2000;
        quote.expires_at = 2060;
        quote.retry_until = 2600;
        quote.unsigned_transaction = transaction;
        quote.unsigned_txid = CTransaction{transaction}.GetHash();
        quote.template_commitment = GetCollaborativeTemplateCommitment(intent, quote, trusted);
        quote.identity_signature.resize(64);
        BOOST_REQUIRE(signer.SignSchnorr(GetPaymasterQuoteSignatureHash(quote),
            quote.identity_signature, nullptr, uint256{}));
        BOOST_REQUIRE_MESSAGE(ValidatePaymasterQuote(quote, intent, policy,
            identity.identity_key, DDCents{100}, 2000, error), error);
        PaymasterQuoteRequest request;
        request.intent = intent;
        PaymasterQuoteResponse response;
        response.request_id = intent.request_id;
        response.session_id = intent.session_id;
        response.quote = quote;
        ProviderAttempt attempt;
        attempt.session_id = intent.session_id;
        attempt.attempt_id = uint256S("f309");
        attempt.provider_id = identity.provider_id;
        attempt.provider_identity_key = identity.identity_key;
        attempt.state = AttemptState::FINAL_COMMITTED;
        attempt.client_nonce = intent.client_nonce;
        attempt.intent_hash = quote.intent_hash;
        attempt.quote_id = quote.quote_id;
        attempt.template_commitment = quote.template_commitment;
        attempt.commit_key = GetPaymasterCommitKey(identity.provider_id, intent.client_nonce,
            quote.intent_hash, quote.quote_id, quote.template_commitment);
        attempt.unsigned_txid = quote.unsigned_txid;
        attempt.quote_request = CanonicalBytes(request);
        attempt.signed_quote = CanonicalBytes(response);
        attempt.unsigned_transaction = CanonicalBytes(transaction);
        attempt.unsigned_psbt = CanonicalBytes(trusted.psbt);
        attempt.input_roles = {ReservationRole::USER_DD, ReservationRole::PROVIDER_DGB};
        attempt.quote_expires_at = quote.expires_at;
        attempt.retry_until = quote.retry_until;
        attempt.created_at = 2000;
        attempt.updated_at = 2001;
        BOOST_REQUIRE_MESSAGE(BuildProviderAuthorizationManifest(intent, quote, trusted,
            uint256S("f30a"), attempt.provider_manifest, error), error);
        const auto txdata = PrecomputePSBTData(trusted.psbt);
        for (unsigned int index{0}; index < transaction.vin.size(); ++index) {
            ScriptExecutionData execution;
            execution.m_annex_init = true;
            execution.m_annex_present = false;
            uint256 sighash;
            BOOST_REQUIRE(SignatureHashSchnorr(sighash, execution, CTransaction{transaction}, index,
                SIGHASH_DEFAULT, SigVersion::TAPROOT, txdata, MissingDataBehavior::FAIL));
            std::vector<unsigned char> signature(64);
            BOOST_REQUIRE((index == 0 ? user_key : spending_key).SignSchnorr(
                sighash, signature, &empty_merkle_root, uint256{}));
            transaction.vin[index].scriptWitness.stack = {signature};
        }
        attempt.final_transaction = CanonicalBytes(transaction);
        attempt.final_txid = CTransaction{transaction}.GetHash();
        ProviderCommitRecord commit;
        commit.commit_key = attempt.commit_key;
        commit.provider_id = identity.provider_id;
        commit.quote_id = quote.quote_id;
        commit.template_commitment = quote.template_commitment;
        commit.final_txid = attempt.final_txid;
        commit.raw_transaction_hash = CTransaction{transaction}.GetWitnessHash();
        commit.final_transaction = attempt.final_transaction;
        commit.provider_inputs = {transaction.vin[1].prevout};
        commit.committed_at = 2001;
        commit.retry_until = quote.retry_until;
        ProviderCommitArtifacts authenticated;
        BOOST_REQUIRE_MESSAGE(ValidateProviderCommitArtifacts(attempt, commit, authenticated, error), error);
        return {std::move(attempt), std::move(commit)};
    }

    std::pair<AlternativeRecoveryRecord, ProviderCommitRecord> Recovery(bool known_pool_funding = false)
    {
        // Storage-boundary fixture: authenticated local recovery authority,
        // not a test of live PSBT signing or chain/mempool admission.
        using paymaster_store::internal::CanonicalBytes;
        LOCK(provider->cs_wallet);
        CKey key;
        ProviderIdentityRecord signer;
        BOOST_REQUIRE(GetPaymasterIdentityKey(*provider, key, signer, error));
        CMutableTransaction funding;
        funding.vin.emplace_back(COutPoint{uint256S("f201"), 0});
        funding.vout.emplace_back(2000, identity.identity_script);
        if (known_pool_funding) {
            // Establish the original pool preparation fee independently of
            // recovery accounting, so this fixture has no older cost gap.
            CMutableTransaction deposit;
            deposit.vin.emplace_back(COutPoint{uint256S("f200"), 0});
            deposit.vout.emplace_back(2007, identity.identity_script);
            const auto incoming = MakeTransactionRef(deposit);
            BOOST_REQUIRE(provider->AddToWallet(incoming,
                TxStateConfirmed{Params().GenesisBlock().GetHash(), 1, 0}));
            funding.vin.front().prevout = COutPoint{incoming->GetHash(), 0};
        }
        const auto parent = MakeTransactionRef(funding);
        BOOST_REQUIRE(provider->AddToWallet(parent, TxStateConfirmed{Params().GenesisBlock().GetHash(), 1, 0}));
        CMutableTransaction user_funding;
        user_funding.vin.emplace_back(COutPoint{uint256S("f202"), 0});
        user_funding.vout.emplace_back(0, identity.identity_script);
        const auto user_parent = MakeTransactionRef(user_funding);
        BOOST_REQUIRE(provider->AddToWallet(user_parent, TxStateConfirmed{Params().GenesisBlock().GetHash(), 1, 0}));
        CMutableTransaction final;
        final.vin.emplace_back(COutPoint{user_parent->GetHash(), 0});
        final.vin.emplace_back(COutPoint{parent->GetHash(), 0});
        final.vout.emplace_back(1500, identity.identity_script);
        AlternativeRecoveryRecord recovery;
        recovery.provider_side = true;
        recovery.request_id = "550e8400-e29b-41d4-a716-446655449004";
        recovery.session_id = uint256S("f203");
        recovery.original_provider_id = uint256S("f204");
        recovery.recovery_provider_id = identity.provider_id;
        recovery.recovery_provider_identity_key = identity.identity_key;
        recovery.recovery_provider_endpoint = "127.0.0.1:18444";
        recovery.client_nonce = uint256S("f205");
        recovery.recovery_id = GetAlternativeRecoveryId(recovery.request_id, recovery.session_id,
            recovery.recovery_provider_id, recovery.client_nonce);
        recovery.offer_id = uint256S("f206");
        recovery.policy_hash = uint256S("f207");
        recovery.original_commit_key = uint256S("f208");
        recovery.original_template_commitment = uint256S("f209");
        recovery.selected_maximum_service_fee = recovery.selected_service_fee = DDCents{100};
        auto& capacity = recovery.capacity_request;
        capacity.genesis_hash = Params().GenesisBlock().GetHash();
        capacity.provider_id = identity.provider_id;
        capacity.request_id = recovery.request_id;
        capacity.session_id = recovery.session_id;
        capacity.client_nonce = recovery.client_nonce;
        capacity.funding_model = FundingModel::USER_PAID;
        capacity.requested_slots = 1;
        capacity.created_at = 2000;
        capacity.expires_at = 2030;
        auto& snapshot = recovery.capacity_snapshot;
        snapshot.snapshot_id = uint256S("f210");
        snapshot.resource_commitment = uint256S("f211");
        snapshot.session_id = recovery.session_id;
        snapshot.provider_id = recovery.recovery_provider_id;
        snapshot.client_nonce = recovery.client_nonce;
        snapshot.attempt_id = recovery.recovery_id;
        snapshot.request_hash = Hash(CanonicalBytes(capacity));
        snapshot.capacity_proof = {1};
        snapshot.created_at = 2000;
        snapshot.validated_at = 2001;
        snapshot.expires_at = 2030;
        snapshot.funding_model = FundingModel::USER_PAID;
        auto& request = recovery.recovery_request;
        request.genesis_hash = capacity.genesis_hash;
        request.request_id = recovery.request_id;
        request.session_id = recovery.session_id;
        request.original_provider_id = recovery.original_provider_id;
        request.recovery_provider_id = recovery.recovery_provider_id;
        request.offer_id = recovery.offer_id;
        request.policy_hash = recovery.policy_hash;
        request.original_commit_key = recovery.original_commit_key;
        request.original_template_commitment = recovery.original_template_commitment;
        request.client_nonce = recovery.client_nonce;
        request.capacity_request = capacity;
        request.capacity_snapshot_id = snapshot.snapshot_id;
        request.capacity_resource_commitment = snapshot.resource_commitment;
        request.user_dd_inputs = {final.vin.front().prevout};
        request.user_input_proofs = {{request.user_dd_inputs.front(), std::vector<unsigned char>(64, 1)}};
        request.wallet_returns = {{identity.identity_script, DDCents{900}}};
        request.maximum_service_fee = request.service_fee = DDCents{100};
        request.created_at = 2000;
        request.expires_at = 2030;
        recovery.recovery_request_hash = GetAlternativeRecoveryRequestHash(request);
        auto& response = recovery.recovery_response;
        response.genesis_hash = capacity.genesis_hash;
        response.request_id = recovery.request_id;
        response.session_id = recovery.session_id;
        response.recovery_id = recovery.recovery_id;
        response.recovery_provider_id = recovery.recovery_provider_id;
        response.recovery_request_hash = recovery.recovery_request_hash;
        response.created_at = 2001;
        response.expires_at = 2025;
        response.unsigned_psbt = {1};
        auto& manifest = response.manifest;
        manifest.request_id = recovery.request_id;
        manifest.session_id = recovery.session_id;
        manifest.original_provider_id = recovery.original_provider_id;
        manifest.recovery_provider_id = recovery.recovery_provider_id;
        manifest.offer_id = recovery.offer_id;
        manifest.policy_hash = recovery.policy_hash;
        manifest.original_commit_key = recovery.original_commit_key;
        manifest.original_template_commitment = recovery.original_template_commitment;
        manifest.capacity_snapshot_id = snapshot.snapshot_id;
        manifest.capacity_resource_commitment = snapshot.resource_commitment;
        manifest.user_dd_inputs = request.user_dd_inputs;
        manifest.wallet_returns = request.wallet_returns;
        manifest.recovery_provider_dgb_inputs = {final.vin.back().prevout};
        manifest.recovery_provider_dd_scripts = {identity.identity_script};
        manifest.recovery_provider_dgb_change_scripts = {identity.identity_script};
        manifest.maximum_service_fee = manifest.service_fee = DDCents{100};
        manifest.network_fee = DGBSatoshis{500};
        manifest.expires_at = response.expires_at;
        manifest.unsigned_txid = CTransaction{final}.GetHash();
        manifest.template_commitment = uint256S("f212");
        manifest.manifest_id = GetAlternativeRecoveryManifestId(manifest);
        response.recovery_commit_key = GetAlternativeRecoveryCommitKey(response);
        response.identity_signature.resize(64);
        BOOST_REQUIRE(key.SignSchnorr(GetAlternativeRecoveryResponseSignatureHash(response),
            response.identity_signature, nullptr, uint256{}));
        BOOST_REQUIRE(BuildRecoveryAuthorizationManifest(request, response, recovery.recovery_authorization, error));
        recovery.provider_safety_policy_hash = uint256S("f213");
        recovery.provider_budget_reservation_id = response.recovery_commit_key;
        recovery.provider_netgroup_bucket = uint256S("f214");
        recovery.provider_maximum_network_fee = manifest.network_fee;
        recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
        recovery.user_signed_psbt = {1};
        recovery.created_at = 2001;
        recovery.updated_at = 2002;
        ProviderBudgetLedger budget;
        budget.recipient_bucket_secret = uint256S("f215");
        budget.accounting_time_high_water = 2002;
        ProviderBudgetReservation reservation;
        reservation.commit_key = response.recovery_commit_key;
        reservation.funding_model = FundingModel::USER_PAID;
        reservation.network_fee = manifest.network_fee;
        reservation.recipient_bucket = GetRecipientBudgetBucket(budget, manifest.wallet_returns.front().script_pub_key);
        reservation.netgroup_bucket = recovery.provider_netgroup_bucket;
        reservation.reserved_at = reservation.updated_at = 2001;
        budget.reservations = {reservation};
        ProviderPoolEntry entry;
        entry.outpoint = final.vin.back().prevout;
        entry.purpose = PoolPurpose::OPERATIONAL;
        entry.state = PoolEntryState::RESERVED;
        entry.script_pub_key = identity.identity_script;
        entry.dgb_value = DGBSatoshis{2000};
        entry.reservation_id = response.recovery_commit_key;
        entry.confirmation_height = 1;
        entry.updated_at = 2001;
        WalletBatch batch{*database};
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(budget));
        BOOST_REQUIRE(batch.WritePaymasterProviderPool({entry}));
        BOOST_REQUIRE(batch.WritePaymasterAlternativeRecovery(recovery));
        recovery.phase = AlternativeRecoveryPhase::FINAL_COMMITTED;
        recovery.final_transaction = CanonicalBytes(final);
        recovery.expected_wtxid = CTransaction{final}.GetWitnessHash();
        recovery.updated_at = 2003;
        auto& result = recovery.signed_result;
        result.genesis_hash = capacity.genesis_hash;
        result.provider_id = identity.provider_id;
        result.commit_key = response.recovery_commit_key;
        result.result_sequence = 1;
        result.status = PaymasterResultStatus::FINAL_COMMITTED;
        result.txid = manifest.unsigned_txid;
        result.raw_transaction_hash = Hash(recovery.final_transaction);
        result.final_transaction = final;
        result.updated_at = 2003;
        result.identity_signature.resize(64);
        BOOST_REQUIRE(key.SignSchnorr(GetPaymasterResultSignatureHash(result), result.identity_signature, nullptr, uint256{}));
        ProviderCommitRecord commit;
        commit.commit_key = response.recovery_commit_key;
        commit.provider_id = identity.provider_id;
        commit.quote_id = recovery.recovery_id;
        commit.template_commitment = manifest.template_commitment;
        commit.final_txid = manifest.unsigned_txid;
        commit.raw_transaction_hash = *result.raw_transaction_hash;
        commit.final_transaction = recovery.final_transaction;
        commit.provider_inputs = manifest.recovery_provider_dgb_inputs;
        commit.committed_at = 2003;
        commit.retry_until = response.expires_at;
        BOOST_REQUIRE_MESSAGE(paymaster_store::internal::ValidateAlternativeRecoveryRecordShape(recovery, error), error);
        return {std::move(recovery), std::move(commit)};
    }

    void Drain()
    {
        for (size_t i = 0; i < 10; ++i) {
            if (ReconcileProviderFinanceReport(*provider, false, error)) return;
            BOOST_REQUIRE_EQUAL(error, "PAYMASTER_FINANCE_RECONCILING");
        }
        BOOST_FAIL("finance work did not drain");
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_wallet_finance_tests, FinanceTestingSetup)

BOOST_AUTO_TEST_CASE(transfer_finance_authenticates_expired_and_conflicted_history)
{
    auto [attempt, commit] = Transfer();
    CMutableTransaction final;
    BOOST_REQUIRE_MESSAGE(ValidateProviderCommitForExecution(attempt, commit,
        std::max(GetTime(), attempt.retry_until + 1), final, error), error);
    attempt.state = AttemptState::CONFLICTED;
    ProviderCommitArtifacts artifacts;
    BOOST_REQUIRE_MESSAGE(ValidateProviderCommitArtifacts(attempt, commit, artifacts, error), error);
    BOOST_CHECK(!ValidateProviderCommitForExecution(attempt, commit,
        std::max(GetTime(), attempt.retry_until + 1), final, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_COMMIT_AUTHORIZATION_STATE");

    WalletBatch batch{*database};
    ProviderFinanceMetadata metadata;
    ProviderFinanceEvent prepared;
    bool changed{false};
    const auto before = database->m_records;
    BOOST_REQUIRE_MESSAGE(paymaster_store::internal::PrepareProviderTransferFinanceEvent(
        batch, identity, attempt, commit, Params().GenesisBlock().GetHash(),
        metadata, prepared, changed, error), error);
    BOOST_CHECK(changed);
    BOOST_CHECK_EQUAL(prepared.dd_income.value, 100);
    BOOST_CHECK_EQUAL(prepared.dgb_cost.value, 500);
    BOOST_CHECK(database->m_records == before);

    PaymentSession session;
    session.request_id = attempt.provider_manifest.request_id;
    session.session_id = attempt.session_id;
    session.attempt_ids = {attempt.attempt_id};
    session.created_at = session.updated_at = attempt.created_at;
    BOOST_REQUIRE(batch.WritePaymasterSession(session));
    BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
    BOOST_REQUIRE(batch.WritePaymasterProviderCommit(commit));
    {
        LOCK(provider->cs_wallet);
        BOOST_REQUIRE(provider->AddToWallet(MakeTransactionRef(artifacts.final_transaction),
            TxStateConfirmed{Params().GenesisBlock().GetHash(), 2, 0}));
    }
    size_t changed_events{0};
    BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(*provider, changed_events, error), error);
    BOOST_CHECK_EQUAL(Metadata().event_count, 1U);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 100);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dgb_cost.value, 500);
    BOOST_CHECK_EQUAL(Metadata().totals.total.successful_transfers, 1U);
    ProviderFinanceEvent stored;
    BOOST_REQUIRE(batch.ReadPaymasterFinanceEvent(commit.commit_key, stored) == DatabaseReadStatus::FOUND);
    BOOST_CHECK(stored.kind == ProviderFinanceEventKind::TRANSFER);
    BOOST_CHECK(stored.funding_model == FundingModel::USER_PAID);
    BOOST_REQUIRE_MESSAGE(paymaster_store::internal::PrepareProviderTransferFinanceEvent(
        batch, identity, attempt, commit, Params().GenesisBlock().GetHash(),
        metadata, prepared, changed, error), error);
    BOOST_CHECK(!changed);

    // Warm the source bindings, leave an EVENT pending, and then prune the
    // authoritative session through the real store transition. The immutable
    // accounting event must continue to work after its commit/attempt vanish.
    session.provider_side = true;
    session.state = SessionState::CONFIRMED;
    session.final_txid = commit.final_txid;
    WITH_LOCK(provider->cs_wallet, provider->SetLastBlockProcessed(300, Params().GenesisBlock().GetHash()));
    BOOST_REQUIRE(batch.WritePaymasterSession(session));
    BOOST_REQUIRE(batch.WritePaymasterSessionId(session.session_id, session.request_id));
    BOOST_REQUIRE(batch.WritePaymasterTemplate(attempt.template_commitment, attempt.attempt_id));
    BOOST_REQUIRE(batch.WritePaymasterUnsignedTx(attempt.unsigned_txid, attempt.attempt_id));
    BOOST_REQUIRE(batch.WritePaymasterProviderCommit(commit, true));
    BOOST_REQUIRE(batch.TxnBegin());
    BOOST_REQUIRE(batch.ErasePaymasterProviderCommit(commit.commit_key));
    BOOST_REQUIRE(batch.ErasePaymasterAttempt(attempt.attempt_id));
    BOOST_REQUIRE(batch.TxnAbort());
    Drain(); // Aborted erases must not discard still-live source bindings.
    BOOST_CHECK_EQUAL(Metadata().event_count, 1U);
    BOOST_REQUIRE(batch.WritePaymasterProviderCommit(commit, true));
    PaymasterStore store{*provider};
    BOOST_REQUIRE_MESSAGE(store.PruneFinalSession(session.request_id, error), error);
    BOOST_CHECK(!batch.ReadPaymasterProviderCommit(commit.commit_key, commit));
    BOOST_CHECK(!batch.ReadPaymasterAttempt(attempt.attempt_id, attempt));
    Drain();
    BOOST_CHECK_EQUAL(Metadata().event_count, 1U);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 100);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dgb_cost.value, 500);
    ForgetPaymasterFinanceCache(*database);
    Drain();
    BOOST_CHECK_EQUAL(Metadata().event_count, 1U);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 100);
}

BOOST_AUTO_TEST_CASE(transfer_finance_rejects_changed_manifest_economics_at_every_ingress)
{
    auto [attempt, commit] = Transfer();
    Drain();
    WalletBatch batch{*database};
    PaymentSession session;
    session.request_id = attempt.provider_manifest.request_id;
    session.session_id = attempt.session_id;
    session.attempt_ids = {attempt.attempt_id};
    session.created_at = session.updated_at = attempt.created_at;
    BOOST_REQUIRE(batch.WritePaymasterSession(session));
    BOOST_REQUIRE(batch.WritePaymasterProviderCommit(commit));
    const auto corrupted = [&](int field) {
        ProviderAttempt changed{attempt};
        if (field == 0) ++changed.provider_manifest.service_fee.value;
        if (field == 1) ++changed.provider_manifest.network_fee.value;
        if (field == 2) changed.provider_manifest.funding_model = FundingModel::SPONSORED;
        // Retain the saved manifest id: a derived event is not an independent
        // authority for the unauthenticated economics underneath that id.
        BOOST_REQUIRE(changed.provider_manifest.manifest_id == attempt.provider_manifest.manifest_id);
        return changed;
    };
    for (const bool prior_event : {false, true}) {
        BOOST_CHECK_EQUAL(Metadata().event_count, prior_event ? 1U : 0U);
        for (int field{0}; field < 3; ++field) {
            const auto changed_attempt = corrupted(field);
            BOOST_REQUIRE(batch.WritePaymasterAttempt(changed_attempt));
            const auto before = database->m_records;
            ProviderFinanceMetadata metadata;
            ProviderFinanceEvent event;
            bool changed{false};
            BOOST_CHECK(!paymaster_store::internal::PrepareProviderTransferFinanceEvent(
                batch, identity, changed_attempt, commit, Params().GenesisBlock().GetHash(),
                metadata, event, changed, error));
            BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTH_MANIFEST_INVALID");
            BOOST_CHECK(!ReconcileProviderFinanceReport(*provider, false, error));
            BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTH_MANIFEST_INVALID");
            BOOST_CHECK(database->m_records == before);
        }
        // Full reconstruction must apply the same authority check even when
        // no earlier event exists to expose an immutable-event conflict.
        for (int field{0}; field < 3; ++field) {
            BOOST_REQUIRE(batch.WritePaymasterAttempt(corrupted(field)));
            const auto before = database->m_records;
            size_t changed{0};
            BOOST_CHECK(!RebuildProviderFinances(*provider, changed, error));
            BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTH_MANIFEST_INVALID");
            BOOST_CHECK(database->m_records == before);
        }
        BOOST_REQUIRE(batch.WritePaymasterAttempt(attempt));
        size_t changed{0};
        BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(*provider, changed, error), error);
        ProviderFinanceEvent event;
        BOOST_REQUIRE(batch.ReadPaymasterFinanceEvent(commit.commit_key, event) == DatabaseReadStatus::FOUND);
        BOOST_CHECK_EQUAL(event.dd_income.value, 100);
        BOOST_CHECK_EQUAL(event.dgb_cost.value, 500);
    }
}

BOOST_AUTO_TEST_CASE(warm_finance_work_does_not_rescan_100_1000_8192_sessions)
{
    size_t stored{0};
    WalletBatch batch{*database};
    for (const size_t count : {100U, 1000U, 8192U}) {
        BOOST_REQUIRE(batch.TxnBegin());
        for (; stored < count; ++stored) {
            PaymentSession session;
            session.request_id = strprintf("550e8400-e29b-41d4-a716-%012u", stored);
            session.session_id = uint256S(strprintf("%064x", stored + 1));
            session.created_at = session.updated_at = 1000;
            BOOST_REQUIRE(batch.WritePaymasterSession(session));
        }
        BOOST_REQUIRE(batch.TxnCommit());
        size_t changed{0};
        BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(*provider, changed, error), error);
        database->reads = {};
        provider->NotifyTransactionChanged(uint256S("ffff"), CT_UPDATED);
        const auto start = std::chrono::steady_clock::now();
        BOOST_REQUIRE_MESSAGE(ReconcileProviderFinanceReport(*provider, false, error), error);
        BOOST_CHECK_EQUAL(database->reads.prefixes, 0U);
        BOOST_CHECK_EQUAL(database->reads.ranges, 1U);
        BOOST_CHECK_EQUAL(database->reads.rows, 0U);
        BOOST_TEST_MESSAGE("One transaction at " << count << " sessions: wallet lock held for at most "
            << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() << " us");
    }
}

BOOST_AUTO_TEST_CASE(warm_reports_with_100000_events_read_only_changed_transactions)
{
    std::vector<ProviderFinanceEvent> events;
    events.reserve(100000);
    for (size_t i = 0; i < 100000; ++i) events.push_back(Event(i));
    WalletBatch batch{*database};
    BOOST_REQUIRE_MESSAGE(batch.WritePaymasterFinanceEvents(Metadata(), events, error), error);
    BOOST_REQUIRE_MESSAGE(ReconcileProviderFinanceReport(*provider, false, error), error);
    database->reads = {};
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) BOOST_REQUIRE(ReconcileProviderFinanceReport(*provider, i != 0, error));
    BOOST_CHECK_EQUAL(database->reads.rows, 0U);
    BOOST_CHECK_EQUAL(database->reads.ranges, 0U);
    BOOST_CHECK_EQUAL(database->reads.prefixes, 0U);
    BOOST_TEST_MESSAGE("Five current finance reports with 100000 events held the wallet lock for at most "
        << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() << " us");
    // Coalescing the same notification cannot turn one change into repeated work.
    for (int i = 0; i < 1000; ++i) provider->NotifyTransactionChanged(events.back().transaction_id, CT_UPDATED);
    BOOST_REQUIRE_MESSAGE(ReconcileProviderFinanceReport(*provider, false, error), error);
    BOOST_CHECK_EQUAL(database->reads.ranges, 1U);
    BOOST_CHECK_EQUAL(database->reads.prefixes, 0U);
    BOOST_CHECK_EQUAL(database->reads.rows, 1U);
    BOOST_CHECK_EQUAL(Metadata().event_count, 100000U);
}

BOOST_AUTO_TEST_CASE(transaction_pages_keep_256_budget_and_retry_failed_commits)
{
    CWalletTx* transaction = Transaction();
    BOOST_REQUIRE(transaction);
    const auto txid = transaction->GetHash();
    std::vector<ProviderFinanceEvent> events;
    for (size_t i = 0; i < 513; ++i) events.push_back(Event(i, txid));
    WalletBatch batch{*database};
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), events, error));
    Drain();
    {
        LOCK(provider->cs_wallet);
        transaction->m_state = TxStateConfirmed{Params().GenesisBlock().GetHash(), 1, 0};
        provider->NotifyTransactionChanged(txid, CT_UPDATED);
    }
    database->reads = {};
    BOOST_CHECK(!ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINANCE_RECONCILING");
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 256 * 3);
    BOOST_CHECK_LE(database->reads.rows, 257U); // Includes the look-ahead index row.
    const auto saved = database->m_records;
    database->m_fail_commit = true;
    BOOST_CHECK(!ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    BOOST_CHECK(database->m_records == saved);
    database->m_fail_commit = false;
    // The independent checkpoint was advanced before the rejected DB commit.
    // Retain the job, but never bypass that fence on an automatic retry.
    BOOST_CHECK(!CheckPaymasterCheckpoint(*database, error));
    BOOST_CHECK(!ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK(database->m_records == saved);
    {
        LOCK(provider->cs_wallet);
        BOOST_CHECK(!AreProviderFinancesCurrent(*provider));
    }
    bool guarded{false};
    BOOST_REQUIRE(MarkPaymasterProviderRestored(*database, guarded, error));
    BOOST_REQUIRE(guarded);
    BOOST_CHECK(!ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINANCE_RECONCILING");
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 512 * 3);
    BOOST_REQUIRE(ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 513 * 3);
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(*provider, error));
    const auto revision = Metadata().revision;
    BOOST_REQUIRE(ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(Metadata().revision, revision);
    for (const TxState& state : {TxState{TxStateConflicted{uint256S("99"), 1}}, TxState{TxStateInactive{true}}, TxState{TxStateInactive{}}}) {
        {
            LOCK(provider->cs_wallet);
            transaction->m_state = state;
            provider->NotifyTransactionChanged(txid, CT_UPDATED);
        }
        Drain();
        BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 0);
    }
    // Deletion also uses a single indexed transaction, and a restore rebuild
    // reconstructs the same financial state without depending on cached work.
    {
        LOCK(provider->cs_wallet);
        provider->mapWallet.erase(txid);
        provider->NotifyTransactionChanged(txid, CT_DELETED);
    }
    Drain();
    const auto before = Metadata();
    ForgetPaymasterFinanceCache(*database);
    BOOST_REQUIRE(ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(Metadata().event_count, before.event_count);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, before.totals.total.dd_income.value);
}

BOOST_AUTO_TEST_CASE(committed_finance_sources_enqueue_but_aborted_writes_do_not)
{
    Drain();
    BOOST_CHECK(WITH_LOCK(provider->cs_wallet, return AreProviderFinancesCurrent(*provider)));
    WalletBatch batch{*database};
    auto event = Event(1);
    event.state = ProviderFinanceEventState::CONFIRMED;
    event.confirmed_at = event.updated_at = 1001;
    BOOST_REQUIRE(batch.TxnBegin());
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), {event}, error));
    BOOST_REQUIRE(batch.TxnAbort());
    BOOST_CHECK(WITH_LOCK(provider->cs_wallet, return AreProviderFinancesCurrent(*provider)));
    BOOST_REQUIRE(ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(Metadata().event_count, 0U);
    database->m_fail_commit = true;
    BOOST_CHECK(!batch.WritePaymasterFinanceEvents(Metadata(), {event}, error));
    database->m_fail_commit = false;
    BOOST_REQUIRE(ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(Metadata().event_count, 0U);
    BOOST_CHECK(!CheckPaymasterCheckpoint(*database, error));
    bool guarded{false};
    BOOST_REQUIRE(MarkPaymasterProviderRestored(*database, guarded, error));
    BOOST_REQUIRE(guarded);
    error.clear();
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), {event}, error));
    BOOST_CHECK(!WITH_LOCK(provider->cs_wallet, return AreProviderFinancesCurrent(*provider)));
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 3);
    BOOST_REQUIRE(ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 0);
    BOOST_CHECK(WITH_LOCK(provider->cs_wallet, return AreProviderFinancesCurrent(*provider)));
    const auto revision = Metadata().revision;
    BOOST_REQUIRE(ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(Metadata().revision, revision); // Observations never requeue themselves.
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(*provider, error));
    BOOST_REQUIRE(database->MakeBatch()->Write(std::string{"pmfinance"}, uint16_t{999}));
    BOOST_CHECK(!ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION");
}

BOOST_AUTO_TEST_CASE(warm_finance_read_errors_never_become_zero_capital)
{
    ProviderPoolEntry entry;
    entry.outpoint = COutPoint{uint256::ONE, 0};
    entry.purpose = PoolPurpose::OPERATIONAL;
    entry.script_pub_key = identity.identity_script;
    entry.dgb_value = DGBSatoshis{1000};
    entry.confirmation_height = 1;
    entry.origin_commit_key = uint256::ONE;
    entry.updated_at = 1000;
    WalletBatch batch{*database};
    BOOST_REQUIRE(batch.WritePaymasterProviderPool({entry}));
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ProviderMaintenanceLedger{}));
    Drain();
    ProviderFinanceQuery query;
    query.period = ProviderFinancePeriod::ALL;
    query.now = 2000;
    const auto initial = CaptureProviderFinanceStatus(*provider, query);
    const auto* snapshot = std::get_if<ProviderFinanceStatus>(&initial);
    BOOST_REQUIRE(snapshot);
    BOOST_CHECK_EQUAL(snapshot->pool_capital.dgb_available.value, 1000);
    const auto before = database->m_records;
    for (const auto& [key, code] : {
            std::pair{DBKeys::PAYMASTER_PROVIDER_POOL, "PAYMASTER_INVALID_PROVIDER_POOL"},
            std::pair{DBKeys::PAYMASTER_MAINTENANCE_LEDGER, "PAYMASTER_INVALID_MAINTENANCE_LEDGER"}}) {
        DataStream encoded;
        encoded << key;
        database->reads = {};
        database->reads.unreadable_key = SerializeData{encoded.begin(), encoded.end()};
        const auto captured = CaptureProviderFinanceStatus(*provider, query);
        const auto* failure = std::get_if<ProviderFinanceStatusFailure>(&captured);
        BOOST_REQUIRE(failure);
        BOOST_CHECK(failure->kind == ErrorKind::WALLET);
        BOOST_CHECK_EQUAL(failure->code, code);
        BOOST_CHECK_EQUAL(database->reads.read_failures, 1U);
        BOOST_CHECK_EQUAL(database->reads.failed_existence_checks, 0U);
        BOOST_CHECK(database->m_records == before);
    }
    database->reads = {};
    const auto recovered = CaptureProviderFinanceStatus(*provider, query);
    BOOST_REQUIRE(std::holds_alternative<ProviderFinanceStatus>(recovered));
    BOOST_CHECK_EQUAL(std::get<ProviderFinanceStatus>(recovered).pool_capital.dgb_available.value, 1000);
    // Genuine absence, unlike a failed read, remains a valid known zero.
    BOOST_REQUIRE(database->MakeBatch()->Erase(DBKeys::PAYMASTER_PROVIDER_POOL));
    BOOST_REQUIRE(database->MakeBatch()->Erase(DBKeys::PAYMASTER_MAINTENANCE_LEDGER));
    const auto absent = CaptureProviderFinanceStatus(*provider, query);
    BOOST_REQUIRE(std::holds_alternative<ProviderFinanceStatus>(absent));
    BOOST_CHECK_EQUAL(std::get<ProviderFinanceStatus>(absent).pool_capital.dgb_available.value, 0);
    BOOST_CHECK_EQUAL(std::get<ProviderFinanceStatus>(absent).pool_capital.pending_maintenance_transactions, 0U);
}

BOOST_AUTO_TEST_CASE(wallet_batch_destruction_discards_uncommitted_finance_notifications)
{
    Drain();
    const auto before = database->m_records;
    // Reuse the owner address: an abandoned batch must not leave notifications
    // that a later, unrelated successful transaction could publish.
    std::optional<WalletBatch> batch;
    batch.emplace(*database);
    BOOST_REQUIRE(batch->TxnBegin());
    BOOST_REQUIRE(batch->WritePaymasterFinanceEvents(Metadata(), {Event(1)}, error));
    batch.reset();
    BOOST_CHECK(database->m_records == before);
    BOOST_CHECK(WITH_LOCK(provider->cs_wallet, return AreProviderFinancesCurrent(*provider)));
    batch.emplace(*database);
    BOOST_REQUIRE(batch->TxnBegin());
    BOOST_REQUIRE(batch->TxnCommit());
    BOOST_CHECK(WITH_LOCK(provider->cs_wallet, return AreProviderFinancesCurrent(*provider)));
    BOOST_CHECK_EQUAL(Metadata().event_count, 0U);
    BOOST_REQUIRE(CheckPaymasterCheckpoint(*database, error));
}

BOOST_AUTO_TEST_CASE(finance_rpc_exposes_retryable_backlog_and_current_completion)
{
    Drain();
    std::vector<ProviderFinanceEvent> events;
    for (size_t i = 0; i < 513; ++i) {
        auto event = Event(i);
        event.state = ProviderFinanceEventState::CONFIRMED;
        event.confirmed_at = event.updated_at = 1001;
        events.push_back(event);
    }
    BOOST_REQUIRE(WalletBatch{*database}.WritePaymasterFinanceEvents(Metadata(), events, error));
    WalletContext context;
    context.chain = m_node.chain.get();
    context.args = m_node.args;
    context.wallets.emplace_back(provider.get(), [](CWallet*) {});
    JSONRPCRequest request;
    request.context = &context;
    request.strMethod = "getpaymasterfinancestatus";
    UniValue options{UniValue::VOBJ};
    options.pushKV("period", "all");
    options.pushKV("include_events", true);
    options.pushKV("limit", 2);
    request.params = UniValue{UniValue::VARR};
    request.params.push_back(options);
    for (int i = 0; i < 2; ++i) {
        BOOST_CHECK_EXCEPTION(getpaymasterfinancestatus().HandleRequest(request), UniValue,
            [](const UniValue& failure) {
                return failure.find_value("code").getInt<int>() == RPC_WALLET_ERROR &&
                    failure.find_value("data").find_value("paymaster_code").get_str() == "PAYMASTER_FINANCE_RECONCILING" &&
                    failure.find_value("data").find_value("disposition").get_str() == "retry_before_execution";
            });
    }
    UniValue result;
    try {
        result = getpaymasterfinancestatus().HandleRequest(request);
    } catch (const UniValue& failure) {
        BOOST_FAIL(failure.write());
    }
    BOOST_CHECK_EQUAL(result.find_value("service_fee_income_cents").getInt<int64_t>(), 0);
    BOOST_CHECK_EQUAL(result.find_value("dgb_operating_cost_satoshis").getInt<int64_t>(), 0);
    BOOST_REQUIRE_EQUAL(result.find_value("events").size(), 2U);
    BOOST_CHECK_EQUAL(result.find_value("events")[0].find_value("state").get_str(), "pending");
    BOOST_CHECK(!result.find_value("next_cursor").get_str().empty());
    BOOST_CHECK_EQUAL(Metadata().event_count, 513U);
}

BOOST_AUTO_TEST_CASE(maintenance_sources_publish_only_after_commit)
{
    Drain();
    ProviderMaintenanceRecord record;
    record.operation_id = uint256S("aa");
    record.plan_id = uint256S("bb");
    record.transaction_id = uint256S("cc");
    record.state = ProviderMaintenanceState::BROADCAST;
    record.maximum_fee = DGBSatoshis{100};
    record.actual_fee = DGBSatoshis{7};
    record.created_at = record.updated_at = 1000;
    {
        LOCK(provider->cs_wallet);
        const auto destination = provider->GetNewDestination(OutputType::BECH32M, "finance maintenance");
        BOOST_REQUIRE(destination);
        ProviderMaintenanceOutput output;
        output.script_pub_key = GetScriptForDestination(*destination);
        output.dgb_value = DGBSatoshis{10000};
        record.outputs = {output};
    }
    ProviderMaintenanceLedger ledger;
    ledger.records = {record};
    WalletBatch batch{*database};
    BOOST_REQUIRE(batch.TxnBegin());
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ledger));
    BOOST_REQUIRE(batch.TxnAbort());
    Drain();
    BOOST_CHECK_EQUAL(Metadata().event_count, 0U);
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ledger));
    database->reads = {};
    Drain();
    BOOST_CHECK_EQUAL(database->reads.rows, 0U);
    BOOST_CHECK_EQUAL(database->reads.prefixes, 0U);
    ProviderFinanceEvent event;
    BOOST_REQUIRE(batch.ReadPaymasterFinanceEvent(record.operation_id, event) == DatabaseReadStatus::FOUND);
    BOOST_CHECK_EQUAL(event.dgb_cost.value, 7);
    BOOST_CHECK(event.state == ProviderFinanceEventState::PENDING);
    ledger.records.front().state = ProviderMaintenanceState::FAILED;
    ++ledger.records.front().updated_at;
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ledger));
    Drain();
    BOOST_REQUIRE(batch.ReadPaymasterFinanceEvent(record.operation_id, event) == DatabaseReadStatus::FOUND);
    BOOST_CHECK(event.state == ProviderFinanceEventState::INVALIDATED);
    BOOST_CHECK_EQUAL(Metadata().event_count, 1U);
}

BOOST_AUTO_TEST_CASE(maintenance_crash_recovery_separates_cost_from_budget_exposure)
{
    const auto record = Maintenance(/*journaled=*/false);
    ProviderMaintenanceLedger ledger;
    ledger.records = {record};
    WalletBatch batch{*database};
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ledger));
    size_t recovered{0};
    BOOST_REQUIRE_MESSAGE(paymaster_app::ReconcileProviderMaintenance(*provider, recovered, error), error);
    BOOST_CHECK_EQUAL(recovered, 1U);
    BOOST_REQUIRE(batch.ReadPaymasterMaintenanceLedger(ledger));
    BOOST_CHECK_EQUAL(ledger.records.front().actual_fee.value, 100);
    ProviderMaintenanceBudgetUsage exposure;
    BOOST_REQUIRE(SummarizeProviderMaintenanceBudget(ledger, GetTime(), exposure, error));
    BOOST_CHECK_EQUAL(exposure.total_last_day.value, 100);
    Drain();
    ProviderFinanceEvent event;
    BOOST_REQUIRE(batch.ReadPaymasterFinanceEvent(record.operation_id, event) == DatabaseReadStatus::FOUND);
    BOOST_CHECK_EQUAL(event.dgb_cost.value, 7);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dgb_cost.value, 7);
    const auto revision = Metadata().revision;
    Drain();
    BOOST_CHECK_EQUAL(Metadata().revision, revision);
    BOOST_REQUIRE(batch.ReadPaymasterMaintenanceLedger(ledger));
    BOOST_CHECK_EQUAL(ledger.records.front().actual_fee.value, 100);
}

BOOST_AUTO_TEST_CASE(recovery_finance_commits_atomically_and_never_counts_a_recipient_payment)
{
    Drain();
    auto [recovery, commit] = Recovery();
    PaymasterStore store{*provider};
    BOOST_REQUIRE_MESSAGE(store.CommitProviderAlternativeRecoveryFinal(recovery, commit,
        recovery.signed_result, Params().GenesisBlock().GetHash(), error), error);
    WalletBatch batch{*database};
    ProviderFinanceEvent event;
    BOOST_REQUIRE(batch.ReadPaymasterFinanceEvent(commit.commit_key, event) == DatabaseReadStatus::FOUND);
    BOOST_CHECK(event.kind == ProviderFinanceEventKind::RECOVERY);
    BOOST_CHECK_EQUAL(event.dd_income.value, 100);
    BOOST_CHECK_EQUAL(event.dgb_cost.value, 500);
    BOOST_CHECK(event.state == ProviderFinanceEventState::PENDING);
    ProviderBudgetLedger budget;
    BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(budget));
    BOOST_REQUIRE_EQUAL(budget.reservations.size(), 1U);
    BOOST_CHECK(budget.reservations.front().state == BudgetReservationState::SPENT);
    CWalletTx* transaction;
    {
        LOCK(provider->cs_wallet);
        transaction = provider->AddToWallet(MakeTransactionRef(*recovery.signed_result.final_transaction),
            TxStateConfirmed{Params().GenesisBlock().GetHash(), 2, 0});
    }
    BOOST_REQUIRE(transaction);
    Drain();
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 100);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dgb_cost.value, 500);
    BOOST_CHECK_EQUAL(Metadata().totals.total.successful_transfers, 0U);
    BOOST_CHECK_EQUAL(Metadata().totals.total.maintenance_transactions, 0U);
    BOOST_CHECK_EQUAL(Metadata().totals.income[0], 0);
    // This fixture deliberately lacks the original pool funding prevout.
    // Recovery accounting cannot make that unrelated older gap complete.
    BOOST_CHECK(Metadata().earlier_history_partial);
    BOOST_REQUIRE_MESSAGE(store.CommitProviderAlternativeRecoveryFinal(recovery, commit,
        recovery.signed_result, Params().GenesisBlock().GetHash(), error), error);
    Drain();
    BOOST_CHECK_EQUAL(Metadata().event_count, 1U);
    std::vector<PaymentSession> sessions;
    BOOST_REQUIRE(batch.ListPaymasterSessions(sessions));
    BOOST_CHECK(sessions.empty());
    {
        LOCK(provider->cs_wallet);
        transaction->m_state = TxStateInactive{};
        BOOST_REQUIRE(batch.WriteTx(*transaction));
        provider->NotifyTransactionChanged(commit.final_txid, CT_UPDATED);
    }
    Drain();
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 0);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dgb_cost.value, 0);
    {
        LOCK(provider->cs_wallet);
        transaction->m_state = TxStateConfirmed{Params().GenesisBlock().GetHash(), 2, 0};
        BOOST_REQUIRE(batch.WriteTx(*transaction));
        provider->NotifyTransactionChanged(commit.final_txid, CT_UPDATED);
    }
    Drain();
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 100);
    BOOST_CHECK_EQUAL(Metadata().event_count, 1U);
    ProviderFinanceQuery query;
    query.period = ProviderFinancePeriod::ALL;
    query.now = GetTime();
    const auto captured = CaptureProviderFinanceStatus(*provider, query);
    BOOST_REQUIRE(std::holds_alternative<ProviderFinanceStatus>(captured));
    BOOST_CHECK_EQUAL(std::get<ProviderFinanceStatus>(captured).average_service_fee[size_t(ProviderFinancePeriod::ALL)].value, 0);

    // Recovery operating income must also stay out of the average when an
    // ordinary recipient payment provides a nonzero denominator.
    const auto ordinary = Transaction();
    BOOST_REQUIRE(ordinary);
    {
        LOCK(provider->cs_wallet);
        ordinary->m_state = TxStateConfirmed{Params().GenesisBlock().GetHash(), 3, 0};
        BOOST_REQUIRE(batch.WriteTx(*ordinary));
    }
    auto transfer = Event(901, ordinary->GetHash());
    transfer.state = ProviderFinanceEventState::CONFIRMED;
    transfer.confirmed_at = transfer.updated_at = 1001;
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), {transfer}, error));
    Drain();
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 103);
    BOOST_CHECK_EQUAL(Metadata().totals.total.successful_transfers, 1U);
    const auto mixed = CaptureProviderFinanceStatus(*provider, query);
    BOOST_REQUIRE(std::holds_alternative<ProviderFinanceStatus>(mixed));
    BOOST_CHECK_EQUAL(std::get<ProviderFinanceStatus>(mixed).average_service_fee[size_t(ProviderFinancePeriod::ALL)].value, 3);
}

BOOST_AUTO_TEST_CASE(older_recovery_commits_rebuild_missing_events_after_reload)
{
    auto [recovery, commit] = Recovery(/*known_pool_funding=*/true);
    PaymasterStore store{*provider};
    BOOST_REQUIRE_MESSAGE(store.CommitProviderAlternativeRecoveryFinal(recovery, commit,
        recovery.signed_result, Params().GenesisBlock().GetHash(), error), error);
    {
        LOCK(provider->cs_wallet);
        BOOST_REQUIRE(provider->AddToWallet(MakeTransactionRef(*recovery.signed_result.final_transaction),
            TxStateConfirmed{Params().GenesisBlock().GetHash(), 2, 0}));
    }
    ProviderFinanceLedger old;
    old.genesis_hash = Params().GenesisBlock().GetHash();
    old.provider_id = identity.provider_id;
    old.history_complete_from = old.updated_at = 1000;
    BOOST_REQUIRE(WalletBatch{*database}.WritePaymasterFinanceLedger(old));
    BOOST_CHECK_EQUAL(Metadata().event_count, 0U);
    CWallet reloaded{m_node.chain.get(), "finance-recovery-reload", DuplicateMockDatabase(*database)};
    BOOST_REQUIRE(reloaded.LoadWallet() == DBErrors::LOAD_OK);
    {
        LOCK(reloaded.cs_wallet);
        reloaded.SetLastBlockProcessed(100, Params().GenesisBlock().GetHash());
    }
    size_t changed{0};
    BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(reloaded, changed, error), error);
    WalletBatch batch{reloaded.GetDatabase()};
    ProviderFinanceMetadata metadata;
    BOOST_REQUIRE(batch.ReadPaymasterFinanceMetadata(metadata) == DatabaseReadStatus::FOUND);
    BOOST_CHECK_EQUAL(metadata.event_count, 2U);
    BOOST_CHECK_EQUAL(metadata.totals.total.dd_income.value, 100);
    BOOST_CHECK_EQUAL(metadata.totals.total.dgb_cost.value, 507);
    BOOST_CHECK_EQUAL(metadata.totals.total.successful_transfers, 0U);
    BOOST_CHECK(!metadata.earlier_history_partial);
    ProviderFinanceEvent event;
    BOOST_REQUIRE(batch.ReadPaymasterFinanceEvent(commit.commit_key, event) == DatabaseReadStatus::FOUND);
    BOOST_CHECK(event.kind == ProviderFinanceEventKind::RECOVERY);
    BOOST_CHECK_EQUAL(event.dgb_cost.value, 500);
    const auto revision = metadata.revision;
    BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(reloaded, changed, error), error);
    BOOST_REQUIRE(batch.ReadPaymasterFinanceMetadata(metadata) == DatabaseReadStatus::FOUND);
    BOOST_CHECK_EQUAL(metadata.revision, revision);
}

BOOST_AUTO_TEST_CASE(recovery_final_commit_failure_cannot_publish_finance_or_spend_budget)
{
    Drain();
    auto [recovery, commit] = Recovery();
    const auto before = database->m_records;
    database->m_fail_commit = true;
    PaymasterStore store{*provider};
    BOOST_CHECK(!store.CommitProviderAlternativeRecoveryFinal(recovery, commit,
        recovery.signed_result, Params().GenesisBlock().GetHash(), error));
    database->m_fail_commit = false;
    BOOST_CHECK(database->m_records == before);
    BOOST_CHECK_EQUAL(Metadata().event_count, 0U);
    WalletBatch batch{*database};
    ProviderFinanceEvent event;
    BOOST_CHECK(batch.ReadPaymasterFinanceEvent(commit.commit_key, event) == DatabaseReadStatus::NOT_FOUND);
    ProviderBudgetLedger budget;
    BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(budget));
    BOOST_CHECK(budget.reservations.front().state == BudgetReservationState::RESERVED);
    AlternativeRecoveryRecord saved;
    BOOST_REQUIRE(batch.ReadPaymasterAlternativeRecovery(recovery.recovery_id, saved));
    BOOST_CHECK(saved.phase == AlternativeRecoveryPhase::USER_SIGNED);
    BOOST_CHECK(!CheckPaymasterCheckpoint(*database, error));
}

BOOST_AUTO_TEST_CASE(derived_maintenance_cost_repair_survives_reload_and_journal_pruning)
{
    const auto record = Maintenance(/*journaled=*/true);
    WalletBatch batch{*database};
    // Emulate a pre-fix persisted event after its old maintenance journal was
    // pruned. Exact wallet prevouts still prove the seven-satoshi cost.
    auto estimate = EstimatedMaintenanceEvent(record);
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), {estimate}, error));
    auto corrected = estimate;
    corrected.dgb_cost = DGBSatoshis{7};
    ++corrected.updated_at;
    BOOST_CHECK(!batch.WritePaymasterFinanceEvents(Metadata(), {corrected}, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINANCE_EVENT_CONFLICT");
    size_t changed{0};
    BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(*provider, changed, error), error);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dgb_cost.value, 7);
    BOOST_CHECK_EQUAL(Metadata().event_count, 1U);
    ProviderFinanceEvent event;
    BOOST_REQUIRE(batch.ReadPaymasterFinanceEvent(record.operation_id, event) == DatabaseReadStatus::FOUND);
    BOOST_CHECK_EQUAL(event.dgb_cost.value, 7);
    CWallet reloaded{m_node.chain.get(), "finance-repaired-reload", DuplicateMockDatabase(*database)};
    BOOST_REQUIRE(reloaded.LoadWallet() == DBErrors::LOAD_OK);
    {
        LOCK(reloaded.cs_wallet);
        reloaded.SetLastBlockProcessed(100, Params().GenesisBlock().GetHash());
    }
    BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(reloaded, changed, error), error);
    ProviderFinanceMetadata metadata;
    BOOST_REQUIRE(WalletBatch{reloaded.GetDatabase()}.ReadPaymasterFinanceMetadata(metadata) == DatabaseReadStatus::FOUND);
    BOOST_CHECK_EQUAL(metadata.event_count, 1U);
    BOOST_CHECK_EQUAL(metadata.totals.total.dgb_cost.value, 7);
}

BOOST_AUTO_TEST_CASE(maintenance_cost_repair_failed_commit_preserves_source_and_old_totals)
{
    const auto record = Maintenance(/*journaled=*/true);
    ProviderMaintenanceLedger ledger;
    ledger.records = {record};
    WalletBatch batch{*database};
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ledger));
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), {EstimatedMaintenanceEvent(record)}, error));
    const auto before = database->m_records;
    database->m_fail_commit = true;
    size_t changed{0};
    BOOST_CHECK(!RebuildProviderFinances(*provider, changed, error));
    database->m_fail_commit = false;
    BOOST_CHECK(database->m_records == before);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dgb_cost.value, 100);
    BOOST_REQUIRE(batch.ReadPaymasterMaintenanceLedger(ledger));
    BOOST_CHECK_EQUAL(ledger.records.front().actual_fee.value, 100);
    // The independent checkpoint must conservatively retain its advance. A
    // guarded copy can repair derived accounting without regaining signing.
    BOOST_CHECK(!CheckPaymasterCheckpoint(*database, error));
    bool guarded{false};
    BOOST_REQUIRE(MarkPaymasterProviderRestored(*database, guarded, error));
    BOOST_REQUIRE(guarded);
    BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(*provider, changed, error), error);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dgb_cost.value, 7);
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(*provider, error));
}

BOOST_AUTO_TEST_CASE(unobserved_maintenance_limit_is_never_reported_as_actual_cost)
{
    auto record = Maintenance(/*journaled=*/true);
    record.transaction_id = uint256S("f999");
    ProviderMaintenanceLedger ledger;
    ledger.records = {record};
    BOOST_REQUIRE(WalletBatch{*database}.WritePaymasterMaintenanceLedger(ledger));
    const auto before = database->m_records;
    size_t changed{0};
    BOOST_CHECK(!RebuildProviderFinances(*provider, changed, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINANCE_MAINTENANCE_FEE_UNAVAILABLE");
    BOOST_CHECK(database->m_records == before);
}

namespace {
std::vector<SerializeData> FinanceKeys(const MockableDatabase& database, const std::string& type)
{
    DataStream prefix;
    prefix << type;
    std::vector<SerializeData> keys;
    for (const auto& [key, value] : database.m_records) {
        if (key.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), key.begin())) keys.push_back(key);
    }
    return keys;
}
} // namespace

BOOST_AUTO_TEST_CASE(finance_rebuild_repairs_derived_records_without_rewriting_events)
{
    CWalletTx* transaction = Transaction();
    BOOST_REQUIRE(transaction);
    transaction->m_state = TxStateConfirmed{Params().GenesisBlock().GetHash(), 1, 0};
    WalletBatch batch{*database};
    auto event = Event(1100, transaction->GetHash());
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), {event}, error));
    Drain();
    const auto intact = database->m_records;
    const auto event_keys = FinanceKeys(*database, "pmfinevent");
    BOOST_REQUIRE_EQUAL(event_keys.size(), 1U);
    const auto event_bytes = intact.at(event_keys.front());
    for (const std::string& type : {"pmfinday", "pmfintime", "pmfintx"}) {
        for (const bool corrupt : {false, true}) {
            const auto revision = Metadata().revision;
            const auto keys = FinanceKeys(*database, type);
            BOOST_REQUIRE_EQUAL(keys.size(), 1U);
            if (corrupt) database->m_records.at(keys.front()) = SerializeData{std::byte{0xff}};
            else database->m_records.erase(keys.front());
            // Rebuild must validate sources independently of these derivations.
            size_t changed{0};
            BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(*provider, changed, error), error);
            BOOST_CHECK_EQUAL(Metadata().revision, revision + 1);
            BOOST_CHECK(database->m_records.at(event_keys.front()) == event_bytes);
            ProviderFinanceLedger restored;
            BOOST_REQUIRE(batch.ReadPaymasterFinanceLedger(restored));
            BOOST_CHECK_EQUAL(restored.events.size(), 1U);
            BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 3);
            const auto repaired = database->m_records;
            BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(*provider, changed, error), error);
            BOOST_CHECK(database->m_records == repaired);
        }
    }
    // Metadata totals are disposable too; retain the immutable identity/time
    // fields and source completeness count while rebuilding the aggregate.
    auto metadata = Metadata();
    metadata.totals = {};
    BOOST_REQUIRE(database->MakeBatch()->Write(DBKeys::PAYMASTER_FINANCE_LEDGER, metadata));
    size_t changed{0};
    BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(*provider, changed, error), error);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 3);
    BOOST_CHECK(database->m_records.at(event_keys.front()) == event_bytes);
    const auto day_keys = FinanceKeys(*database, "pmfinday");
    BOOST_REQUIRE_EQUAL(day_keys.size(), 1U);
    ProviderFinanceTotals future_day;
    DataStream decoded;
    decoded.write(database->m_records.at(day_keys.front()));
    decoded >> future_day;
    ++future_day.total.version;
    DataStream encoded;
    encoded << future_day;
    database->m_records.at(day_keys.front()) = SerializeData{encoded.begin(), encoded.end()};
    const auto future = database->m_records;
    BOOST_CHECK(!RebuildProviderFinances(*provider, changed, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION");
    BOOST_CHECK(database->m_records == future);
}

BOOST_AUTO_TEST_CASE(finance_rebuild_refuses_damaged_or_unknown_event_sources)
{
    WalletBatch batch{*database};
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), {Event(1200)}, error));
    Drain();
    const auto intact = database->m_records;
    const auto keys = FinanceKeys(*database, "pmfinevent");
    BOOST_REQUIRE_EQUAL(keys.size(), 1U);
    for (const bool unknown_version : {false, true}) {
        database->m_records = intact;
        if (unknown_version) {
            auto event = Event(1200);
            ++event.version;
            DataStream encoded;
            encoded << event;
            database->m_records.at(keys.front()) = SerializeData{encoded.begin(), encoded.end()};
        } else database->m_records.at(keys.front()) = SerializeData{std::byte{0xff}};
        const auto damaged = database->m_records;
        size_t changed{0};
        BOOST_CHECK(!RebuildProviderFinances(*provider, changed, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_FINANCE_EVENT");
        BOOST_CHECK(database->m_records == damaged);
    }
    database->m_records = intact;
    BOOST_REQUIRE(database->MakeBatch()->Write(DBKeys::PAYMASTER_FINANCE_LEDGER,
        uint16_t(ProviderFinanceMetadata::CURRENT_VERSION + 1)));
    const auto future = database->m_records;
    size_t changed{0};
    BOOST_CHECK(!RebuildProviderFinances(*provider, changed, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION");
    BOOST_CHECK(database->m_records == future);
}

BOOST_AUTO_TEST_CASE(finance_derived_repair_failed_commit_preserves_sources_and_guard)
{
    WalletBatch batch{*database};
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), {Event(1300)}, error));
    Drain();
    const auto keys = FinanceKeys(*database, "pmfintime");
    BOOST_REQUIRE_EQUAL(keys.size(), 1U);
    database->m_records.erase(keys.front());
    const auto damaged = database->m_records;
    database->m_fail_commit = true;
    size_t changed{0};
    BOOST_CHECK(!RebuildProviderFinances(*provider, changed, error));
    database->m_fail_commit = false;
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    BOOST_CHECK(database->m_records == damaged);
    BOOST_CHECK(!CheckPaymasterCheckpoint(*database, error));
    BOOST_CHECK(!WITH_LOCK(provider->cs_wallet, return AreProviderFinancesCurrent(*provider)));
}

BOOST_AUTO_TEST_CASE(warm_reorg_cannot_acknowledge_missing_transaction_index_rows)
{
    CWalletTx* transaction = Transaction();
    BOOST_REQUIRE(transaction);
    transaction->m_state = TxStateConfirmed{Params().GenesisBlock().GetHash(), 1, 0};
    WalletBatch batch{*database};
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), {Event(1400, transaction->GetHash())}, error));
    Drain();
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 3);
    const auto keys = FinanceKeys(*database, "pmfintx");
    BOOST_REQUIRE_EQUAL(keys.size(), 1U);
    database->m_records.erase(keys.front());
    transaction->m_state = TxStateConflicted{uint256S("ff14"), 1};
    provider->NotifyTransactionChanged(transaction->GetHash(), CT_UPDATED);
    BOOST_CHECK(!ReconcileProviderFinanceReport(*provider, false, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINANCE_TRANSACTION_INDEX_READ_FAILED");
    BOOST_CHECK(!WITH_LOCK(provider->cs_wallet, return AreProviderFinancesCurrent(*provider)));
    BOOST_REQUIRE_MESSAGE(ReconcileProviderFinanceReport(*provider, false, error), error);
    BOOST_CHECK_EQUAL(Metadata().totals.total.dd_income.value, 0);
    BOOST_CHECK_EQUAL(FinanceKeys(*database, "pmfintx").size(), 1U);
}

BOOST_AUTO_TEST_CASE(warm_report_index_errors_retry_through_source_repair)
{
    WalletBatch batch{*database};
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), {Event(1450)}, error));
    Drain();
    const auto keys = FinanceKeys(*database, "pmfintime");
    BOOST_REQUIRE_EQUAL(keys.size(), 1U);
    database->m_records.at(keys.front()) = SerializeData{std::byte{0xff}};
    ProviderFinanceQuery query;
    query.now = 2000;
    query.period = ProviderFinancePeriod::ALL;
    query.include_events = true;
    auto captured = CaptureProviderFinanceStatus(*provider, query);
    const auto* failure = std::get_if<ProviderFinanceStatusFailure>(&captured);
    BOOST_REQUIRE(failure);
    BOOST_CHECK_EQUAL(failure->code, "PAYMASTER_FINANCE_INDEX_INVALID");
    BOOST_CHECK(!WITH_LOCK(provider->cs_wallet, return AreProviderFinancesCurrent(*provider)));
    captured = CaptureProviderFinanceStatus(*provider, query);
    const auto* status = std::get_if<ProviderFinanceStatus>(&captured);
    BOOST_REQUIRE(status);
    BOOST_CHECK_EQUAL(status->report.events.size(), 1U);
}

BOOST_AUTO_TEST_CASE(warm_reports_detect_missing_time_and_day_records_without_full_scans)
{
    CWalletTx* transaction = Transaction();
    BOOST_REQUIRE(transaction);
    transaction->m_state = TxStateConfirmed{Params().GenesisBlock().GetHash(), 1, 0};
    WalletBatch batch{*database};
    const auto event = Event(1470, transaction->GetHash());
    BOOST_REQUIRE(batch.WritePaymasterFinanceEvents(Metadata(), {event}, error));
    Drain();
    ProviderFinanceEvent confirmed;
    BOOST_REQUIRE(batch.ReadPaymasterFinanceEvent(event.event_id, confirmed) == DatabaseReadStatus::FOUND);
    BOOST_REQUIRE(confirmed.state == ProviderFinanceEventState::CONFIRMED);
    ProviderFinanceQuery query;
    // The fixture confirms in the genesis block; inspect that calendar window
    // rather than today's chart, which legitimately excludes the old bucket.
    query.now = confirmed.confirmed_at;
    query.period = ProviderFinancePeriod::ALL;
    query.include_events = true;
    const auto intact = CaptureProviderFinanceStatus(*provider, query);
    const auto* initial = std::get_if<ProviderFinanceStatus>(&intact);
    BOOST_REQUIRE(initial);
    BOOST_REQUIRE_EQUAL(initial->report.days.size(), 1U);
    BOOST_CHECK_EQUAL(initial->report.periods[size_t(ProviderFinancePeriod::TODAY)].total.dd_income.value, 3);
    for (const std::string& type : {"pmfintime", "pmfinday"}) {
        BOOST_TEST_CONTEXT(type) {
            const auto keys = FinanceKeys(*database, type);
            BOOST_REQUIRE_EQUAL(keys.size(), 1U);
            database->m_records.erase(keys.front());
            database->reads = {};
            const auto captured = CaptureProviderFinanceStatus(*provider, query);
            const auto* failure = std::get_if<ProviderFinanceStatusFailure>(&captured);
            BOOST_REQUIRE(failure);
            BOOST_CHECK_EQUAL(failure->code, "PAYMASTER_FINANCE_INDEX_INVALID");
            BOOST_CHECK_EQUAL(database->reads.prefixes, 0U);
            BOOST_CHECK_LE(database->reads.rows, 2U);
            const auto restored = CaptureProviderFinanceStatus(*provider, query);
            const auto* status = std::get_if<ProviderFinanceStatus>(&restored);
            BOOST_REQUIRE(status);
            BOOST_CHECK_EQUAL(status->report.events.size(), 1U);
            BOOST_CHECK_EQUAL(status->report.periods[size_t(ProviderFinancePeriod::ALL)].total.dd_income.value, 3);
        }
    }
    // A structurally valid but wrong summary is not evidence of known zero.
    auto metadata = Metadata();
    metadata.totals = {};
    BOOST_REQUIRE(database->MakeBatch()->Write(DBKeys::PAYMASTER_FINANCE_LEDGER, metadata));
    ProviderFinanceMetadata summary;
    std::string summary_error;
    database->reads = {};
    BOOST_CHECK(!ReadProviderFinanceSummary(*provider, summary, summary_error));
    BOOST_CHECK_EQUAL(summary_error, "PAYMASTER_FINANCE_INDEX_INVALID");
    BOOST_CHECK_EQUAL(database->reads.prefixes, 0U);
    BOOST_CHECK_EQUAL(database->reads.rows, 0U);
    BOOST_REQUIRE(std::holds_alternative<ProviderFinanceStatus>(CaptureProviderFinanceStatus(*provider, query)));
    BOOST_REQUIRE(ReadProviderFinanceSummary(*provider, summary, summary_error));
    BOOST_CHECK_EQUAL(summary.totals.total.dd_income.value, 3);
    BOOST_REQUIRE(database->MakeBatch()->Write(DBKeys::PAYMASTER_FINANCE_LEDGER, metadata));
    BOOST_REQUIRE(std::holds_alternative<ProviderFinanceStatusFailure>(CaptureProviderFinanceStatus(*provider, query)));
    BOOST_REQUIRE(std::holds_alternative<ProviderFinanceStatus>(CaptureProviderFinanceStatus(*provider, query)));
}

BOOST_AUTO_TEST_CASE(failed_signed_maintenance_remains_open_in_finance_status)
{
    auto record = Maintenance(/*journaled=*/true);
    record.state = ProviderMaintenanceState::FAILED;
    ProviderMaintenanceLedger ledger;
    ledger.records = {record};
    BOOST_REQUIRE(WalletBatch{*database}.WritePaymasterMaintenanceLedger(ledger));
    ProviderFinanceQuery query;
    query.now = 2000;
    auto captured = CaptureProviderFinanceStatus(*provider, query);
    const auto* status = std::get_if<ProviderFinanceStatus>(&captured);
    BOOST_REQUIRE(status);
    BOOST_CHECK_EQUAL(status->pool_capital.pending_maintenance_transactions, 1U);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
