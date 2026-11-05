// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.

#include <wallet/paymasterapplication_internal.h>
#include <wallet/paymasterclient.h>
#include <wallet/rpc/paymaster_internal.h>
#include <base58.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <index/txindex.h>
#include <key.h>
#include <paymaster/directory.h>
#include <script/standard.h>
#include <streams.h>
#include <test/util/index.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <validation.h>
#include <validationinterface.h>
#include <version.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <vector>

namespace wallet {
namespace {
using namespace DigiDollar::Paymaster;
using namespace paymaster_app;
using namespace paymaster_rpc::internal;

BOOST_FIXTURE_TEST_SUITE(paymaster_application_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(retry_request_binding_preserves_historical_restricted_artifacts)
{
    CKey key;
    key.MakeNewKey(true);
    const XOnlyPubKey identity_key{key.GetPubKey()};
    const CScript recipient = GetScriptForDestination(WitnessV1Taproot{identity_key});
    const std::vector<COutPoint> inputs{{uint256S("01"), 0}, {uint256S("02"), 1}};
    ClientPaymentOptions options;
    options.request_id = "550e8400-e29b-41d4-a716-446655440001";
    options.address = EncodeDigiDollarAddress(WitnessV1Taproot{identity_key});
    options.fee_cap = 3;
    options.maximum_attempts = 1;
    options.identity_key = std::vector<unsigned char>{identity_key.begin(), identity_key.end()};
    RestrictedServiceDescriptor descriptor;
    descriptor.genesis_hash = Params().GenesisBlock().GetHash();
    descriptor.provider_id = GetPaymasterId(identity_key);
    descriptor.offer_id = uint256S("03");
    descriptor.policy_hash = uint256S("04");
    descriptor.sponsor_authorization_key = identity_key;
    descriptor.expires_at = 100;
    SponsorshipCapability capability;
    capability.genesis_hash = descriptor.genesis_hash;
    capability.provider_id = descriptor.provider_id;
    capability.offer_id = descriptor.offer_id;
    capability.policy_hash = descriptor.policy_hash;
    capability.recipient_script = recipient;
    capability.amount = DDCents{500};
    capability.payment_request_nonce = uint256S("05");
    capability.expires_at = 100;
    const auto encode = [](const auto& value) {
        CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
        stream << value;
        const auto bytes = MakeUCharSpan(stream);
        return std::vector<unsigned char>{bytes.begin(), bytes.end()};
    };
    options.descriptor = encode(descriptor);
    options.capability = encode(capability);
    const uint256 original = CanonicalClientRequestHash(
        options.request_id, recipient, DDCents{500}, DDCents{3}, inputs,
        options.mode, options.privacy, options.selection, options.maximum_attempts,
        false, false, identity_key, descriptor, capability);
    // Binding reconstruction neither validates an expired descriptor as a new
    // offer nor requires current Restricted issuance or provider policy.
    BOOST_CHECK(CanonicalClientRequestHash(options, DDCents{500}, inputs) == original);
    auto changed = options;
    ++changed.fee_cap;
    BOOST_CHECK(CanonicalClientRequestHash(changed, DDCents{500}, inputs) != original);
    changed = options;
    ++changed.maximum_attempts;
    BOOST_CHECK(CanonicalClientRequestHash(changed, DDCents{500}, inputs) != original);
    auto reversed_inputs = inputs;
    std::reverse(reversed_inputs.begin(), reversed_inputs.end());
    BOOST_CHECK(CanonicalClientRequestHash(options, DDCents{500}, reversed_inputs) != original);
    ++capability.expires_at;
    changed = options;
    changed.capability = encode(capability);
    BOOST_CHECK(CanonicalClientRequestHash(changed, DDCents{500}, inputs) != original);
    BOOST_CHECK(CanonicalClientRequestHash(options, DDCents{500}, inputs) == original);
}

BOOST_FIXTURE_TEST_CASE(active_tip_resolution_rejects_empty_unknown_and_inactive_locators,
                        TestChain100Setup)
{
    auto& chain = *m_node.chain;
    const CBlockLocator locator = chain.getTipLocator();
    const auto tip = ResolvePaymasterActiveTip(chain, locator);
    BOOST_REQUIRE(tip);
    BOOST_CHECK_EQUAL(tip->height, 100);
    BOOST_CHECK(tip->hash == locator.vHave.front());
    BOOST_CHECK(!ResolvePaymasterActiveTip(chain, CBlockLocator{}));

    CBlockLocator unknown = locator;
    unknown.vHave.front() = uint256::ONE;
    // Known ancestors following an unknown first entry grant no readiness.
    BOOST_CHECK(!ResolvePaymasterActiveTip(chain, unknown));

    const CBlockLocator genesis = WITH_LOCK(cs_main,
        return GetLocator(m_node.chainman->ActiveChain().Genesis()));
    const auto genesis_tip = ResolvePaymasterActiveTip(chain, genesis);
    BOOST_REQUIRE(genesis_tip);
    BOOST_CHECK_EQUAL(genesis_tip->height, 0);
    BOOST_CHECK(genesis_tip->hash == Params().GenesisBlock().GetHash());

    CBlockIndex* invalidated = WITH_LOCK(cs_main,
        return m_node.chainman->ActiveChain().Tip());
    BlockValidationState state;
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, invalidated));
    // Reproduce a disconnect between locator capture and hash resolution.
    // The old first hash is known, but its still-active ancestors are no fallback.
    BOOST_CHECK(!ResolvePaymasterActiveTip(chain, locator));
    const auto shortened = ResolvePaymasterActiveTip(chain, chain.getTipLocator());
    BOOST_REQUIRE(shortened);
    BOOST_CHECK_EQUAL(shortened->height, tip->height - 1);
}

BOOST_FIXTURE_TEST_CASE(active_tip_resolution_handles_extension_and_same_height_replacement,
                        TestChain100Setup)
{
    auto& chain = *m_node.chain;
    const CBlockLocator original_locator = chain.getTipLocator();
    const auto original = ResolvePaymasterActiveTip(chain, original_locator);
    BOOST_REQUIRE(original);

    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    // Extension leaves the captured block active without making it the latest tip.
    const auto retained = ResolvePaymasterActiveTip(chain, original_locator);
    BOOST_REQUIRE(retained);
    BOOST_CHECK(retained->hash == original->hash);
    BOOST_CHECK_EQUAL(retained->height, original->height);
    const CBlockLocator extended_locator = chain.getTipLocator();
    const auto extended = ResolvePaymasterActiveTip(chain, extended_locator);
    BOOST_REQUIRE(extended);
    BOOST_CHECK_EQUAL(extended->height, original->height + 1);
    BOOST_CHECK(extended->hash != retained->hash);

    CBlockIndex* invalidated = WITH_LOCK(cs_main,
        return m_node.chainman->ActiveChain().Tip());
    BlockValidationState state;
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, invalidated));
    coinbaseKey.MakeNewKey(true);
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    const auto replacement = ResolvePaymasterActiveTip(chain, chain.getTipLocator());
    BOOST_REQUIRE(replacement);
    BOOST_CHECK_EQUAL(replacement->height, extended->height);
    BOOST_CHECK(replacement->hash != extended->hash);
    BOOST_CHECK(!ResolvePaymasterActiveTip(chain, extended_locator));
}

BOOST_FIXTURE_TEST_CASE(provider_readiness_requires_matching_wallet_and_index_tip,
                        TestChain100Setup)
{
    BOOST_REQUIRE(!g_txindex);
    struct IndexCleanup {
        ~IndexCleanup()
        {
            if (g_txindex) g_txindex->Interrupt();
            SyncWithValidationInterfaceQueue();
            if (g_txindex) g_txindex->Stop();
            g_txindex.reset();
        }
    } index_cleanup;
    CWallet wallet{m_node.chain.get(), "paymaster-readiness", CreateMockableWalletDatabase()};
    BOOST_REQUIRE(wallet.LoadWallet() == DBErrors::LOAD_OK);
    const auto tip = ResolvePaymasterActiveTip(*m_node.chain, m_node.chain->getTipLocator());
    BOOST_REQUIRE(tip);
    {
        LOCK(wallet.cs_wallet);
        wallet.SetLastBlockProcessed(tip->height, tip->hash);
    }
    BOOST_CHECK(!AutomaticProviderStateIsSynchronized(wallet));
    BOOST_CHECK(!ProviderTxIndexIsReady(wallet, false));
    BOOST_CHECK(!ProviderTxIndexIsReady(wallet, true));

    g_txindex = std::make_unique<TxIndex>(interfaces::MakeChain(m_node), 1 << 20, true);
    BOOST_REQUIRE(g_txindex->Init());
    BOOST_CHECK(!AutomaticProviderStateIsSynchronized(wallet));
    BOOST_CHECK(!ProviderTxIndexIsReady(wallet, false));
    BOOST_CHECK(!ProviderTxIndexIsReady(wallet, true));
    BOOST_REQUIRE(g_txindex->StartBackgroundSync());
    IndexWaitSynced(*g_txindex);
    BOOST_CHECK(AutomaticProviderStateIsSynchronized(wallet));
    BOOST_CHECK(ProviderTxIndexIsReady(wallet, false));
    BOOST_CHECK(ProviderTxIndexIsReady(wallet, true));

    {
        LOCK(wallet.cs_wallet);
        wallet.SetLastBlockProcessed(tip->height, uint256::ONE);
    }
    BOOST_CHECK(!AutomaticProviderStateIsSynchronized(wallet));
    BOOST_CHECK(ProviderTxIndexIsReady(wallet, false));
    {
        LOCK(wallet.cs_wallet);
        wallet.SetLastBlockProcessed(tip->height - 1, tip->hash);
    }
    BOOST_CHECK(!AutomaticProviderStateIsSynchronized(wallet));
    BOOST_CHECK(ProviderTxIndexIsReady(wallet, false));

    // Retain a real, synchronized index at the old tip. No timing assumption
    // about notification delivery can make the stale-index checks pass.
    SyncWithValidationInterfaceQueue();
    g_txindex->Stop();
    CBlockIndex* invalidated = WITH_LOCK(cs_main,
        return m_node.chainman->ActiveChain().Tip());
    BlockValidationState state;
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, invalidated));
    coinbaseKey.MakeNewKey(true);
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    const auto replacement = ResolvePaymasterActiveTip(*m_node.chain, m_node.chain->getTipLocator());
    BOOST_REQUIRE(replacement);
    BOOST_REQUIRE_EQUAL(replacement->height, tip->height);
    BOOST_REQUIRE(replacement->hash != tip->hash);
    {
        LOCK(wallet.cs_wallet);
        wallet.SetLastBlockProcessed(replacement->height, replacement->hash);
    }
    const IndexSummary stale = g_txindex->GetSummary();
    BOOST_REQUIRE(stale.synced);
    BOOST_REQUIRE_EQUAL(stale.best_block_height, replacement->height);
    BOOST_REQUIRE(stale.best_block_hash == tip->hash);
    BOOST_CHECK(!AutomaticProviderStateIsSynchronized(wallet));
    BOOST_CHECK(!ProviderTxIndexIsReady(wallet, false));
}

BOOST_AUTO_TEST_CASE(maintenance_rpc_budget_matches_authorization_after_clock_rollback)
{
    CKey key;
    key.MakeNewKey(true);
    TaprootBuilder builder;
    builder.Finalize(XOnlyPubKey{key.GetPubKey()});
    ProviderMaintenanceOutput output;
    output.purpose = PoolPurpose::OPERATIONAL;
    output.asset = PoolAsset::DGB;
    output.script_pub_key = GetScriptForDestination(builder.GetOutput());
    output.dgb_value = DGBSatoshis{1000};

    ProviderReadiness readiness;
    readiness.have_liquidity_policy = true;
    readiness.have_maintenance_ledger = true;
    auto& policy = readiness.liquidity_policy;
    policy.paid_maintenance_approved = true;
    policy.maximum_maintenance_fee_per_transaction = DGBSatoshis{100};
    policy.maximum_maintenance_fee_per_hour = DGBSatoshis{100};
    policy.maximum_maintenance_fee_per_day = DGBSatoshis{200};
    policy.updated_at = 100000;
    ProviderMaintenanceRecord confirmed;
    confirmed.operation_id = uint256S("01");
    confirmed.plan_id = uint256S("02");
    confirmed.outputs = {output};
    confirmed.maximum_fee = DGBSatoshis{100};
    confirmed.actual_fee = DGBSatoshis{100};
    confirmed.state = ProviderMaintenanceState::CONFIRMED;
    confirmed.transaction_id = uint256S("03");
    confirmed.created_at = confirmed.updated_at = 100000;
    readiness.maintenance_ledger.records = {confirmed};

    for (const int64_t high_water : {107200, 186401}) {
        BOOST_TEST_CONTEXT("accounting time floor " << high_water) {
            // A policy update advanced the durable clock; wall time then
            // moved backwards. The already confirmed spend is unchanged.
            readiness.maintenance_ledger.accounting_time_high_water = high_water;
            const auto status = ProviderLiquidityStatusToJSON(readiness, 100000);
            BOOST_CHECK_EQUAL(status.find_value("maintenance_fee_spent_last_hour_satoshis").getInt<int64_t>(), 0);
            BOOST_CHECK_EQUAL(status.find_value("maintenance_fee_spent_last_day_satoshis").getInt<int64_t>(),
                              high_water == 107200 ? 100 : 0);
            BOOST_CHECK_EQUAL(status.find_value("maintenance_fee_reserved_satoshis").getInt<int64_t>(), 0);

            auto candidate = confirmed;
            candidate.operation_id = uint256S("04");
            candidate.plan_id = uint256S("05");
            candidate.state = ProviderMaintenanceState::PLANNED;
            candidate.actual_fee = DGBSatoshis{0};
            candidate.transaction_id.SetNull();
            auto authorized_ledger = readiness.maintenance_ledger;
            std::string error;
            BOOST_REQUIRE(ReserveProviderMaintenanceBudget(authorized_ledger, policy, candidate, 100000, error));
            BOOST_CHECK_EQUAL(readiness.maintenance_ledger.records.size(), 1U);
            BOOST_CHECK_EQUAL(readiness.maintenance_ledger.records.front().updated_at, 100000);
            BOOST_CHECK_EQUAL(authorized_ledger.records.back().created_at, high_water);
        }
    }
}

BOOST_AUTO_TEST_CASE(resume_terms_preserve_present_zero_and_false_values)
{
    PaymentResult persisted;
    persisted.provider_id = uint256S("01");
    persisted.payment_cents = DDCents{900};
    persisted.service_fee_cents = DDCents{15};
    persisted.subtract_paymaster_fee_from_amount = true;
    persisted.authorization_commitment = uint256S("02");
    persisted.authorization_accepted = true;
    persisted.psbt = std::vector<unsigned char>{1, 2, 3};

    PaymentResult current;
    current.service_fee_cents = DDCents{0};
    current.subtract_paymaster_fee_from_amount = false;
    current.authorization_commitment = uint256S("03");
    FillMissingPaymentTerms(current, persisted);
    BOOST_REQUIRE(current.payment_cents);
    BOOST_CHECK_EQUAL(current.payment_cents->value, 900);
    BOOST_REQUIRE(current.provider_id);
    BOOST_CHECK(*current.provider_id == *persisted.provider_id);
    BOOST_REQUIRE(current.service_fee_cents);
    BOOST_CHECK_EQUAL(current.service_fee_cents->value, 0);
    BOOST_REQUIRE(current.subtract_paymaster_fee_from_amount.has_value());
    BOOST_CHECK(!*current.subtract_paymaster_fee_from_amount);
    BOOST_CHECK(*current.authorization_commitment == uint256S("03"));
    // Display completion must not import an accepted authorization or signed bytes.
    BOOST_CHECK(!current.authorization_accepted.has_value());
    BOOST_CHECK(!current.psbt.has_value());
}

BOOST_AUTO_TEST_CASE(payment_rpc_preserves_units_false_values_and_artifacts)
{
    PaymentResult payment;
    payment.request_id = "11111111-1111-4111-8111-111111111111";
    payment.payment_cents = DDCents{12345};
    payment.service_fee_cents = DDCents{0};
    payment.user_total_cents = DDCents{12345};
    payment.funding_model = FundingModel::SPONSORED;
    payment.authorization_accepted = false;
    payment.processed = false;
    payment.queued = false;
    payment.psbt = std::vector<unsigned char>{0x70, 0x73, 0x62, 0x74, 0xff, 0x00};
    payment.txid = uint256S("01");
    const UniValue json = ToJSON(payment);
    BOOST_CHECK_EQUAL(json.find_value("payment_cents").getInt<int64_t>(), 12345);
    BOOST_CHECK_EQUAL(json.find_value("service_fee_cents").getInt<int64_t>(), 0);
    BOOST_CHECK_EQUAL(json.find_value("user_total_cents").getInt<int64_t>(), 12345);
    BOOST_CHECK_EQUAL(json.find_value("funding_model").get_str(), "sponsored");
    BOOST_CHECK(json.find_value("processed").isBool());
    BOOST_CHECK(!json.find_value("processed").get_bool());
    BOOST_CHECK(json.find_value("authorization_accepted").isBool());
    BOOST_CHECK(!json.find_value("authorization_accepted").get_bool());
    BOOST_CHECK(json.find_value("queued").isBool());
    BOOST_CHECK(!json.find_value("queued").get_bool());
    BOOST_CHECK(!json.exists("authorization_accepted_at"));
    BOOST_CHECK(!json.exists("broadcast"));
    BOOST_CHECK_EQUAL(json.find_value("psbt").get_str(), "cHNidP8A");
    BOOST_CHECK_EQUAL(json.find_value("txid").get_str(), uint256S("01").GetHex());
}

BOOST_AUTO_TEST_CASE(provider_start_and_deferred_work_keep_native_control_values)
{
    ProviderStartResult start;
    start.operation_mode = ProviderOperationMode::AUTOMATIC;
    start.service_state = ProviderServiceState::WAITING_FOR_READINESS;
    start.readiness_errors = {"PAYMASTER_INVALID_MAINTENANCE_LEDGER"};
    start.recovery.candidates = 2;
    start.recovery.recovered = 1;
    start.recovery.errors = {"PAYMASTER_RECOVERY_RETRY_REQUIRED"};
    const UniValue json = ToJSON(start);
    BOOST_CHECK(!start.running);
    BOOST_CHECK_EQUAL(json.find_value("running").get_bool(), start.running);
    BOOST_CHECK_EQUAL(json.find_value("readiness_errors")[0].get_str(), start.readiness_errors.front());
    BOOST_CHECK_EQUAL(json.find_value("service_state").get_str(), "waiting_for_readiness");
    BOOST_CHECK_EQUAL(json.find_value("recovery_candidates").getInt<int>(), 2);
    BOOST_CHECK_EQUAL(json.find_value("recovered_broadcasts").getInt<int>(), 1);
    BOOST_CHECK_EQUAL(json.find_value("expired_commits").getInt<int>(), 0);
    BOOST_CHECK(!json.exists("provider_id"));
    BOOST_CHECK(!json.exists("endpoint"));
    BOOST_CHECK(!json.exists("transport"));

    ProviderProcessingResult deferred;
    deferred.processed = true;
    deferred.queued = false;
    deferred.deferred = true;
    deferred.message_type = "capacity";
    deferred.deferral_reason = "PAYMASTER_PROVIDER_BUSY";
    const UniValue response = ToJSON(deferred);
    BOOST_CHECK(deferred.deferred.value_or(false));
    BOOST_CHECK(response.find_value("deferred").get_bool());
    BOOST_CHECK(!response.find_value("queued").get_bool());
    BOOST_CHECK(!response.exists("commit"));
    BOOST_CHECK(!response.exists("recovery"));
    BOOST_CHECK(!response.exists("txid"));
}

BOOST_AUTO_TEST_CASE(application_error_rpc_retains_disposition_and_absence)
{
    try {
        InvokePaymasterService([]() -> UniValue {
            throw ApplicationError(ErrorKind::WALLET, "PAYMASTER_SESSION_NOT_FOUND",
                                   ErrorDisposition::RECONCILE, true);
        });
        BOOST_FAIL("application error was not propagated");
    } catch (const UniValue& error) {
        BOOST_CHECK_EQUAL(error.find_value("code").getInt<int>(), RPC_WALLET_ERROR);
        BOOST_CHECK_EQUAL(error.find_value("message").get_str(), "PAYMASTER_SESSION_NOT_FOUND");
        const auto& data = error.find_value("data");
        BOOST_CHECK_EQUAL(data.find_value("paymaster_code").get_str(), "PAYMASTER_SESSION_NOT_FOUND");
        BOOST_CHECK_EQUAL(data.find_value("disposition").get_str(),
                          ErrorDispositionName(ErrorDisposition::RECONCILE));
        BOOST_CHECK(data.find_value("session_absent").get_bool());
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace
} // namespace wallet
