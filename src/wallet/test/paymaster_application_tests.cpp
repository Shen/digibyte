// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.

#include <wallet/paymasterapplication_internal.h>
#include <wallet/paymasterclient.h>
#include <wallet/paymasterfinance.h>
#include <wallet/digidollarwallet.h>
#include <wallet/rpc/paymaster_internal.h>
#include <base58.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <hash.h>
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
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace wallet {
namespace {
using namespace DigiDollar::Paymaster;
using namespace paymaster_app;
using namespace paymaster_rpc::internal;

BOOST_FIXTURE_TEST_SUITE(paymaster_application_tests, BasicTestingSetup)

BOOST_FIXTURE_TEST_CASE(client_order_replay_uses_persisted_terms_and_existing_hash, WalletTestingSetup)
{
    {
        LOCK(m_wallet.cs_wallet);
        const auto* tip = WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE(tip);
        m_wallet.SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    }
    CKey key;
    key.MakeNewKey(true);
    ClientPaymentOptions original;
    original.request_id = "550e8400-e29b-41d4-a716-446655440021";
    original.address = EncodeDigiDollarAddress(WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
    original.fee_cap = 57;
    original.maximum_attempts = 1;
    original.selection = SelectionMode::PRIVACY_WEIGHTED;
    const std::vector<COutPoint> inputs{{uint256S("01"), 3}, {uint256S("02"), 7}};
    auto order = BuildClientPaymentOrder(original, DDCents{325}, inputs);
    BOOST_REQUIRE(order);
    // Preserve the old exact tag, field order and widths across extraction.
    HashWriter before = TaggedHash("DigiByte Paymaster RPC Request v1");
    before << original.request_id << order->recipient_script << DDCents{325} << DDCents{57} << inputs
           << static_cast<uint8_t>(original.mode) << static_cast<uint8_t>(original.privacy)
           << static_cast<uint8_t>(original.selection) << original.maximum_attempts;
    BOOST_CHECK(before.GetSHA256() == order->canonical_request_hash);

    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    BOOST_CHECK(store.CreateOrJoinSession(original.request_id, order->canonical_request_hash,
        original.mode, GetTime(), session, error, &*order) == CreatePaymasterSessionResult::CREATED);
    BOOST_CHECK(session.user_inputs.empty()); // Replay metadata reserves/signs nothing.
    BOOST_CHECK(session.attempt_ids.empty());
    BOOST_CHECK(session.state == SessionState::CREATED);

    // Use fresh native accessors and the actual database codec/serializer,
    // never a mock that accepts arbitrary send options.
    PaymasterStore reopened{m_wallet};
    PaymentSession restored_session;
    BOOST_REQUIRE(reopened.GetSessionByRequestId(original.request_id, restored_session));
    auto snapshot = CaptureSessionSnapshot(restored_session, &reopened);
    BOOST_REQUIRE_MESSAGE(snapshot.resume_order, snapshot.resume_order_error);
    const auto valid_order = *snapshot.resume_order;
    const auto encoded = SessionToJSON(restored_session, &reopened);
    BOOST_REQUIRE(encoded.find_value("resume_order_available").isTrue());
    const auto& replay = encoded.find_value("resume_order");
    ClientPaymentOptions restored;
    restored.request_id = replay.find_value("request_id").get_str();
    restored.address = replay.find_value("to_address").get_str();
    restored.fee_cap = replay.find_value("maximum_paymaster_fee_cents").getInt<int64_t>();
    restored.maximum_attempts = replay.find_value("maximum_provider_attempts").getInt<int>();
    restored.selection = replay.find_value("selection").get_str() == "privacy_weighted"
        ? SelectionMode::PRIVACY_WEIGHTED : SelectionMode::LOWEST_TOTAL_COST;
    BOOST_CHECK_EQUAL(restored.fee_cap, 57);
    BOOST_CHECK_EQUAL(restored.maximum_attempts, 1);
    BOOST_CHECK(restored.selection == SelectionMode::PRIVACY_WEIGHTED);
    BOOST_CHECK_EQUAL(replay.find_value("selected_inputs")[0].find_value("vout").getInt<int>(), 3);
    BOOST_CHECK_EQUAL(replay.find_value("selected_inputs")[1].find_value("vout").getInt<int>(), 7);
    const auto replay_hash = CanonicalClientRequestHash(restored, DDCents{325}, inputs);
    BOOST_CHECK(replay_hash == restored_session.canonical_request_hash);
    BOOST_CHECK(reopened.CreateOrJoinSession(restored.request_id, replay_hash, restored.mode,
        GetTime(), restored_session, error) == CreatePaymasterSessionResult::JOINED);
    for (int field = 0; field < 3; ++field) {
        auto changed = restored;
        if (field == 0) changed.fee_cap = 100;
        if (field == 1) changed.maximum_attempts = 3;
        if (field == 2) changed.selection = SelectionMode::LOWEST_TOTAL_COST;
        BOOST_CHECK(reopened.CreateOrJoinSession(restored.request_id,
            CanonicalClientRequestHash(changed, DDCents{325}, inputs), changed.mode,
            GetTime(), restored_session, error) == CreatePaymasterSessionResult::CONFLICT);
        BOOST_CHECK_EQUAL(error, "PAYMASTER_REQUEST_ID_CONFLICT");
    }

    PaymentIntent intent;
    intent.genesis_hash = Params().GenesisBlock().GetHash();
    intent.request_id = session.request_id;
    intent.session_id = session.session_id;
    intent.canonical_request_hash = session.canonical_request_hash;
    intent.recipient_script = order->recipient_script;
    intent.user_dd_inputs = inputs;
    intent.requested_fee_mode = original.mode;
    intent.privacy_profile = original.privacy;
    intent.selection_mode = original.selection;
    BOOST_CHECK(ValidateClientPaymentOrder(*snapshot.resume_order, session, &intent,
        Params().GenesisBlock().GetHash(), error));
    intent.selection_mode = SelectionMode::LOWEST_TOTAL_COST;
    BOOST_CHECK(!ValidateClientPaymentOrder(*snapshot.resume_order, session, &intent,
        Params().GenesisBlock().GetHash(), error));

    const auto record_key = std::make_pair(DBKeys::PAYMASTER_CLIENT_ORDER, original.request_id);
    auto corrupt = *snapshot.resume_order;
    corrupt.maximum_service_fee = DDCents{100}; // Keep hash; invent different replay terms.
    BOOST_REQUIRE(m_wallet.GetDatabase().MakeBatch()->Write(record_key, corrupt));
    snapshot = CaptureSessionSnapshot(session, &reopened);
    BOOST_CHECK(!snapshot.resume_order);
    BOOST_CHECK_EQUAL(snapshot.resume_order_error, "PAYMASTER_CLIENT_ORDER_BINDING_MISMATCH");
    BOOST_CHECK(ToJSON(snapshot).find_value("session_state").get_str() == "CREATED");
    BOOST_REQUIRE(m_wallet.GetDatabase().MakeBatch()->Write(record_key, uint16_t{65535}));
    snapshot = CaptureSessionSnapshot(session, &reopened);
    BOOST_CHECK(!snapshot.resume_order);
    BOOST_CHECK_EQUAL(snapshot.resume_order_error, "PAYMASTER_CLIENT_ORDER_VERSION_UNSUPPORTED");
    BOOST_CHECK(snapshot.observation_requested); // Optional metadata never hides status.
    BOOST_REQUIRE(m_wallet.GetDatabase().MakeBatch()->Erase(record_key));
    snapshot = CaptureSessionSnapshot(session, &reopened);
    BOOST_CHECK(!snapshot.resume_order);
    BOOST_CHECK_EQUAL(snapshot.resume_order_error, "PAYMASTER_CLIENT_ORDER_UNAVAILABLE");
    BOOST_CHECK(reopened.CreateOrJoinSession(restored.request_id, replay_hash, restored.mode,
        GetTime(), restored_session, error) == CreatePaymasterSessionResult::JOINED);

    BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.WritePaymasterClientOrder(valid_order));
    session.state = SessionState::FAILED;
    BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.WritePaymasterSession(session));
    BOOST_REQUIRE_MESSAGE(reopened.PruneFinalSession(original.request_id, error), error);
    ClientPaymentOrder pruned_order;
    BOOST_CHECK(reopened.GetClientPaymentOrderWithStatus(original.request_id, pruned_order) == DatabaseReadStatus::NOT_FOUND);
    BOOST_REQUIRE(reopened.GetSessionByRequestId(original.request_id, restored_session));
    BOOST_CHECK(restored_session.canonical_request_hash == replay_hash); // Tombstone identity survives detail pruning.

    auto invented = valid_order;
    invented.request_id = "550e8400-e29b-41d4-a716-446655440022";
    BOOST_CHECK(store.CreateOrJoinSession(invented.request_id, invented.canonical_request_hash,
        original.mode, GetTime(), restored_session, error, &invented) == CreatePaymasterSessionResult::CONFLICT);
    BOOST_CHECK(!store.GetSessionByRequestId(invented.request_id, restored_session));
}

BOOST_FIXTURE_TEST_CASE(created_sweep_resume_selects_original_unreserved_wallet_inputs, WalletTestingSetup)
{
    DigiDollarWallet dd_wallet{&m_wallet};
    CKey key;
    key.MakeNewKey(true);
    const CScript script = GetScriptForDestination(WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
    const auto add_confirmed_input = [&](const uint256& nonce, CAmount amount) {
        CMutableTransaction tx;
        tx.vin.emplace_back(COutPoint{nonce, 0});
        tx.vout.emplace_back(0, script);
        const auto transaction = MakeTransactionRef(std::move(tx));
        {
            LOCK(m_wallet.cs_wallet);
            const auto* tip = WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip());
            BOOST_REQUIRE(tip);
            m_wallet.SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
            m_wallet.AddToWallet(transaction, TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, 0});
        }
        const COutPoint input{transaction->GetHash(), 0};
        dd_wallet.AddDDUTXO(input, amount);
        return input;
    };
    const auto first = add_confirmed_input(uint256S("21"), 1600);
    const auto second = add_confirmed_input(uint256S("22"), 1400);
    // Deliberately preserve the request's order rather than outpoint sorting.
    const std::vector<COutPoint> original_inputs{second, first};
    ClientPaymentOptions original;
    original.request_id = "550e8400-e29b-41d4-a716-446655440023";
    original.address = EncodeDigiDollarAddress(WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
    original.subtract_fee = true;
    original.send_all = true;
    auto order = BuildClientPaymentOrder(original, DDCents{3000}, original_inputs);
    BOOST_REQUIRE(order);
    PaymasterStore store{m_wallet};
    PaymentSession session;
    std::string error;
    BOOST_REQUIRE(store.CreateOrJoinSession(original.request_id, order->canonical_request_hash,
        original.mode, GetTime(), session, error, &*order) == CreatePaymasterSessionResult::CREATED);
    BOOST_REQUIRE(session.user_inputs.empty()); // Crash before ReserveInputs.
    const auto new_input = add_confirmed_input(uint256S("23"), 2000);
    BOOST_REQUIRE_EQUAL(dd_wallet.GetSpendableDDBalance(), 5000);
    std::vector<COutPoint> selected;
    std::vector<CAmount> amounts;
    CAmount total{0};
    // This is the same ordinary-wallet selection helper used by automatic
    // resume. It is a planner regression, not a signed/network payment fixture.
    auto result = SelectCreatedClientSweepInputs(m_wallet, dd_wallet, store, session,
        DDCents{3000}, selected, total, &amounts, error);
    BOOST_REQUIRE(result.has_value());
    BOOST_REQUIRE_MESSAGE(*result, error);
    BOOST_CHECK(selected == original_inputs);
    BOOST_CHECK_EQUAL(total, 3000);
    BOOST_CHECK((amounts == std::vector<CAmount>{1400, 1600}));
    BOOST_CHECK(std::find(selected.begin(), selected.end(), new_input) == selected.end());
    BOOST_CHECK(CanonicalClientRequestHash(original, DDCents{3000}, selected) == session.canonical_request_hash);
    BOOST_CHECK(!IsPaymasterInputReserved(m_wallet, first));
    BOOST_CHECK(!IsPaymasterInputReserved(m_wallet, second));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.LockCoin(first));
    }
    result = SelectCreatedClientSweepInputs(m_wallet, dd_wallet, store, session,
        DDCents{3000}, selected, total, &amounts, error);
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK(!*result);
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INPUT_LOCKED");
    BOOST_CHECK(selected.empty());
    BOOST_CHECK_EQUAL(total, 0);
    BOOST_CHECK(amounts.empty());
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.UnlockCoin(first));
    }
    PaymentSession other;
    BOOST_REQUIRE(store.CreateOrJoinSession("550e8400-e29b-41d4-a716-446655440024",
        uint256S("24"), FeeMode::PAYMASTER, GetTime(), other, error) == CreatePaymasterSessionResult::CREATED);
    BOOST_REQUIRE(store.ReserveInputs(other.request_id, {{first, ReservationRole::USER_DD}},
        FeeMode::PAYMASTER, GetTime(), error));
    result = SelectCreatedClientSweepInputs(m_wallet, dd_wallet, store, session,
        DDCents{3000}, selected, total, &amounts, error);
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK(!*result); // Metadata cannot borrow another session's reservation.
    BOOST_CHECK(selected.empty());
    BOOST_CHECK(IsPaymasterInputReserved(m_wallet, first));
    BOOST_REQUIRE(store.GetSessionByRequestId(original.request_id, session));
    BOOST_CHECK(session.user_inputs.empty());
    BOOST_CHECK(session.attempt_ids.empty());
    BOOST_CHECK(session.state == SessionState::CREATED);
    BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.ErasePaymasterClientOrder(original.request_id));
    BOOST_CHECK(!SelectCreatedClientSweepInputs(m_wallet, dd_wallet, store, session,
        DDCents{3000}, selected, total, &amounts, error).has_value());
    BOOST_CHECK(error.empty()); // Missing metadata keeps legacy exact replay available.
}

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
    deferred.queued = false;
    deferred.disposition = ProviderProcessingDisposition::DEFERRED;
    deferred.message_type = "capacity";
    deferred.deferral_reason = "PAYMASTER_PROVIDER_BUSY";
    const UniValue response = ToJSON(deferred);
    BOOST_CHECK(deferred.IsDeferred());
    BOOST_CHECK(response.find_value("deferred").get_bool());
    BOOST_CHECK(!response.find_value("queued").get_bool());
    BOOST_CHECK(!response.exists("commit"));
    BOOST_CHECK(!response.exists("recovery"));
    BOOST_CHECK(!response.exists("txid"));
}

BOOST_AUTO_TEST_CASE(provider_processing_disposition_preserves_rpc_contract)
{
    for (const auto disposition : {ProviderProcessingDisposition::NO_MESSAGE,
                                  ProviderProcessingDisposition::PROCESSED,
                                  ProviderProcessingDisposition::REJECTED,
                                  ProviderProcessingDisposition::DEFERRED}) {
        ProviderProcessingResult result;
        result.disposition = disposition;
        result.rejection_reason = "PAYMASTER_INVALID_QUOTE_REQUEST";
        result.deferral_reason = "PAYMASTER_PROVIDER_BUSY";
        result.result_status = PaymasterResultStatus::REJECTED;
        for (const std::optional<bool> delivery : {std::optional<bool>{}, std::optional<bool>{false}, std::optional<bool>{true}}) {
            result.queued = delivery;
            const auto json = ToJSON(result);
            BOOST_CHECK_EQUAL(json.find_value("processed").get_bool(), disposition != ProviderProcessingDisposition::NO_MESSAGE);
            BOOST_CHECK_EQUAL(json.exists("rejected"), disposition == ProviderProcessingDisposition::REJECTED);
            BOOST_CHECK_EQUAL(json.exists("deferred"), disposition == ProviderProcessingDisposition::DEFERRED);
            BOOST_CHECK_EQUAL(json.exists("rejection_reason"), result.IsRejected());
            BOOST_CHECK_EQUAL(json.exists("deferral_reason"), result.IsDeferred());
            if (json.exists("rejected")) BOOST_CHECK(json.find_value("rejected").get_bool());
            if (json.exists("deferred")) BOOST_CHECK(json.find_value("deferred").get_bool());
            BOOST_CHECK_EQUAL(json.exists("queued"), delivery.has_value());
            if (delivery) BOOST_CHECK_EQUAL(json.find_value("queued").get_bool(), *delivery);
            BOOST_CHECK_EQUAL(json.find_value("result_status").get_str(), "rejected");
        }
    }
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

BOOST_AUTO_TEST_CASE(native_finance_and_automation_json_preserve_complete_observations)
{
    ProviderFinanceStatus finance;
    finance.query.period = ProviderFinancePeriod::ALL;
    finance.provider_id = uint256::ONE;
    finance.report.metadata.revision = 7;
    finance.report.metadata.history_complete_from = 100;
    auto& total = finance.report.periods[size_t(ProviderFinancePeriod::ALL)];
    total.total.dd_income = DDCents{15};
    total.total.dgb_cost = DGBSatoshis{7};
    total.total.successful_transfers = total.total.user_paid_transfers = 3;
    total.income[0] = 15;
    total.cost[0] = 7;
    finance.average_service_fee[size_t(ProviderFinancePeriod::ALL)] = DDCents{5};
    UniValue json = ToJSON(finance);
    BOOST_CHECK_EQUAL(json.find_value("period").get_str(), "all");
    BOOST_CHECK_EQUAL(json.find_value("revision").getInt<uint64_t>(), 7U);
    BOOST_CHECK_EQUAL(json.find_value("service_fee_income_cents").getInt<int64_t>(), 15);
    BOOST_CHECK_EQUAL(json.find_value("dgb_operating_cost_satoshis").getInt<int64_t>(), 7);
    BOOST_CHECK_EQUAL(json.find_value("average_service_fee_cents").getInt<int64_t>(), 5);
    BOOST_CHECK(json.find_value("backup_required").isFalse());
    BOOST_CHECK_EQUAL(json.find_value("pool_capital").find_value("carrier_withdrawable_cents").getInt<int64_t>(), 0);
    BOOST_CHECK_EQUAL(json.find_value("period_summaries").find_value("today").find_value("successful_transfers").getInt<uint64_t>(), 0U);
    BOOST_CHECK_EQUAL(json.find_value("model_breakdown").find_value("public_sponsored").find_value("service_fee_income_cents").getInt<int64_t>(), 0);
    for (const auto* field : {"oracle_price_micro_usd", "valuation_time", "estimated_result_usd", "daily_totals", "events", "next_cursor"}) {
        BOOST_CHECK(!json.exists(field));
    }
    // Unknown valuation is omitted; a known zero estimate remains a number.
    finance.valuation = ProviderFinanceValuation{1000000, 12345, 0.0};
    finance.query.include_events = true;
    finance.report.next_cursor = "abcd";
    ProviderFinanceEvent event;
    event.event_id = uint256S("02");
    event.transaction_id = uint256S("03");
    event.kind = ProviderFinanceEventKind::TRANSFER;
    event.state = ProviderFinanceEventState::CONFIRMED;
    event.funding_model = FundingModel::SPONSORED;
    event.sponsorship_scope = SponsorshipScope::PUBLIC;
    event.created_at = 100;
    event.confirmed_at = 101;
    finance.report.events.push_back(event);
    json = ToJSON(finance);
    BOOST_CHECK(json.find_value("estimated_result_usd").isNum());
    BOOST_CHECK_EQUAL(json.find_value("estimated_result_usd").get_real(), 0.0);
    BOOST_CHECK_EQUAL(json.find_value("valuation_time").getInt<int64_t>(), 12345);
    BOOST_CHECK(json.find_value("daily_totals").isArray());
    BOOST_CHECK_EQUAL(json.find_value("events").size(), 1U);
    BOOST_CHECK_EQUAL(json.find_value("events")[0].find_value("funding_model").get_str(), "sponsored");
    BOOST_CHECK_EQUAL(json.find_value("events")[0].find_value("sponsorship_scope").get_str(), "public");
    BOOST_CHECK_EQUAL(json.find_value("next_cursor").get_str(), "abcd");

    ProviderAutomationSnapshot automation;
    json = ToJSON(automation);
    BOOST_CHECK(json.find_value("configured").isFalse());
    BOOST_CHECK(json.find_value("enabled").isFalse());
    BOOST_CHECK(json.find_value("paid_maintenance_approved").isFalse());
    BOOST_CHECK(json.find_value("pending_transaction").isFalse());
    for (const auto& [state, name] : std::vector<std::pair<ProviderAutomationState, const char*>>{
             {ProviderAutomationState::READY, "ready"}, {ProviderAutomationState::OFF, "off"},
             {ProviderAutomationState::BLOCKED, "blocked"}, {ProviderAutomationState::PAUSED, "paused"},
             {ProviderAutomationState::WAITING_CONFIRMATION, "waiting_confirmation"}, {ProviderAutomationState::WORKING, "working"}}) {
        automation.state = state;
        BOOST_CHECK_EQUAL(ToJSON(automation).find_value("state").get_str(), name);
    }
}

BOOST_FIXTURE_TEST_CASE(pool_refresh_uses_current_reservations_instead_of_caller_snapshot, WalletTestingSetup)
{
    LOCK(m_wallet.cs_wallet);
    CKey key;
    key.MakeNewKey(true);
    const CScript script = GetScriptForDestination(WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
    CMutableTransaction funding;
    funding.vin.emplace_back(COutPoint{uint256::ONE, 0});
    for (int i = 0; i < 3; ++i) funding.vout.emplace_back(MIN_ADMISSION_DGB_SATOSHIS, script);
    BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(funding),
        TxStateConfirmed{Params().GenesisBlock().GetHash(), 1, 0}));
    std::vector<ProviderPoolEntry> current;
    for (uint32_t i = 0; i < 2; ++i) {
        ProviderPoolEntry entry;
        entry.outpoint = COutPoint{funding.GetHash(), i};
        entry.script_pub_key = script;
        entry.dgb_value = DGBSatoshis{MIN_ADMISSION_DGB_SATOSHIS};
        entry.updated_at = 1000;
        current.push_back(entry);
    }
    WalletBatch batch{m_wallet.GetDatabase()};
    BOOST_REQUIRE(batch.WritePaymasterProviderPool(current));
    auto stale = current;
    // Interleave a new reservation and another pool insertion after status read.
    current.front().state = PoolEntryState::RESERVED;
    current.front().reservation_id = uint256S("ab");
    auto added = current.back();
    added.outpoint.n = 2;
    current.push_back(added);
    BOOST_REQUIRE(batch.WritePaymasterProviderPool(current));
    std::string error;
    BOOST_REQUIRE_MESSAGE(RefreshPoolConfirmationHeights(m_wallet, stale, error), error);
    BOOST_REQUIRE_EQUAL(stale.size(), 3U);
    BOOST_CHECK(stale.front().state == PoolEntryState::RESERVED);
    BOOST_CHECK(stale.front().reservation_id == uint256S("ab"));
    BOOST_CHECK_EQUAL(stale.back().confirmation_height, 1);
    BOOST_REQUIRE(batch.ReadPaymasterProviderPool(current));
    BOOST_CHECK(current.front().state == PoolEntryState::RESERVED);
    BOOST_CHECK(current.front().reservation_id == uint256S("ab"));
    BOOST_REQUIRE_EQUAL(current.size(), 3U);
    stale.front().state = PoolEntryState::AVAILABLE;
    stale.front().reservation_id.SetNull();
    BOOST_REQUIRE(RefreshPoolConfirmationHeights(m_wallet, stale, error, false));
    BOOST_REQUIRE(batch.ReadPaymasterProviderPool(current));
    BOOST_CHECK(current.front().state == PoolEntryState::RESERVED);
}

BOOST_FIXTURE_TEST_CASE(maintenance_history_is_bounded_and_observes_later_wallet_transactions, WalletTestingSetup)
{
    LOCK(m_wallet.cs_wallet);
    m_wallet.SetLastBlockProcessed(100, Params().GenesisBlock().GetHash());
    CKey key;
    key.MakeNewKey(true);
    const CScript script = GetScriptForDestination(WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
    ProviderMaintenanceOutput output;
    output.script_pub_key = script;
    output.dgb_value = DGBSatoshis{MIN_ADMISSION_DGB_SATOSHIS};
    ProviderMaintenanceRecord record;
    record.operation_id = uint256S("11");
    record.plan_id = uint256S("12");
    record.outputs = {output};
    record.maximum_fee = DGBSatoshis{100};
    record.created_at = record.updated_at = 1000;
    ProviderMaintenanceLedger ledger;
    ledger.records.push_back(record);
    WalletBatch batch{m_wallet.GetDatabase()};
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ledger));
    for (uint32_t i = 0; i < 600; ++i) {
        CMutableTransaction unrelated;
        unrelated.vin.emplace_back(COutPoint{uint256::ONE, i});
        unrelated.vout.emplace_back(1, CScript{} << OP_TRUE);
        BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(unrelated), TxStateInactive{}));
    }
    size_t recovered{0};
    std::string error;
    ProviderLiquidityPolicy policy;
    BOOST_CHECK(!RunAutomaticDGBReplenishment(m_wallet, policy, 0, 1, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_RECONCILING");
    BOOST_CHECK_EQUAL(recovered, 0U);
    BOOST_CHECK(!RunAutomaticCarrierReplenishment(m_wallet, policy, 0, 1, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_RECONCILING");
    BOOST_REQUIRE_MESSAGE(ReconcileProviderMaintenance(m_wallet, recovered, error), error);
    // A completed negative lookup is cheap until the wallet changes.
    BOOST_REQUIRE(ReconcileProviderMaintenance(m_wallet, recovered, error));
    BOOST_CHECK_EQUAL(recovered, 0U);
    // A rehash invalidates traversal order; restart instead of skipping history.
    m_wallet.mapWallet.rehash(m_wallet.mapWallet.bucket_count() * 2);
    BOOST_CHECK(!ReconcileProviderMaintenance(m_wallet, recovered, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_RECONCILING");
    BOOST_CHECK(!ReconcileProviderMaintenance(m_wallet, recovered, error));
    BOOST_REQUIRE_MESSAGE(ReconcileProviderMaintenance(m_wallet, recovered, error), error);
    // AddToWallet can retain an in-memory insertion when WriteTx fails before
    // NotifyTransactionChanged. A matching bucket count cannot hide that change.
    CMutableTransaction unnotified;
    unnotified.vin.emplace_back(COutPoint{uint256S("14"), 0});
    unnotified.vout.emplace_back(1, CScript{} << OP_TRUE);
    m_wallet.mapWallet.try_emplace(unnotified.GetHash(), MakeTransactionRef(unnotified), TxStateInactive{});
    BOOST_CHECK(!ReconcileProviderMaintenance(m_wallet, recovered, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_RECONCILING");
    BOOST_CHECK(!ReconcileProviderMaintenance(m_wallet, recovered, error));
    BOOST_REQUIRE_MESSAGE(ReconcileProviderMaintenance(m_wallet, recovered, error), error);
    CMutableTransaction completed;
    completed.vin.emplace_back(COutPoint{uint256S("13"), 0});
    completed.vout.emplace_back(output.dgb_value.value, script);
    BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(completed), TxStateInactive{}));
    BOOST_REQUIRE_MESSAGE(ReconcileProviderMaintenance(m_wallet, recovered, error), error);
    BOOST_CHECK_EQUAL(recovered, 1U);
    BOOST_REQUIRE(batch.ReadPaymasterMaintenanceLedger(ledger));
    BOOST_CHECK(ledger.records.front().transaction_id == completed.GetHash());
    BOOST_CHECK(ledger.records.front().state == ProviderMaintenanceState::BROADCAST);
    BOOST_REQUIRE(ReconcileProviderMaintenance(m_wallet, recovered, error));
    BOOST_CHECK_EQUAL(recovered, 0U);
}

BOOST_FIXTURE_TEST_CASE(maintenance_history_never_recovers_an_ambiguous_match, WalletTestingSetup)
{
    LOCK(m_wallet.cs_wallet);
    CKey key;
    key.MakeNewKey(true);
    const CScript script = GetScriptForDestination(WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
    ProviderMaintenanceOutput output;
    output.script_pub_key = script;
    output.dgb_value = DGBSatoshis{MIN_ADMISSION_DGB_SATOSHIS};
    ProviderMaintenanceRecord record;
    record.operation_id = uint256S("21");
    record.plan_id = uint256S("22");
    record.outputs = {output};
    record.maximum_fee = DGBSatoshis{100};
    record.created_at = record.updated_at = 1000;
    ProviderMaintenanceLedger ledger;
    ledger.records.push_back(record);
    WalletBatch batch{m_wallet.GetDatabase()};
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ledger));
    for (uint32_t i = 0; i < 3; ++i) {
        CMutableTransaction candidate;
        candidate.vin.emplace_back(COutPoint{uint256::ONE, i});
        candidate.vout.emplace_back(output.dgb_value.value, script);
        BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(candidate), TxStateInactive{}));
    }
    size_t recovered{0};
    std::string error;
    BOOST_CHECK(!ReconcileProviderMaintenance(m_wallet, recovered, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_TRANSACTION_AMBIGUOUS");
    BOOST_CHECK_EQUAL(recovered, 0U);
    BOOST_REQUIRE(batch.ReadPaymasterMaintenanceLedger(ledger));
    BOOST_CHECK(ledger.records.front().transaction_id.IsNull());
    BOOST_CHECK(ledger.records.front().state == ProviderMaintenanceState::PLANNED);
    // Only two candidates are retained. Removing one must rescan for the
    // omitted third candidate before any remaining match can become authority.
    for (size_t remaining = 2; remaining > 0; --remaining) {
        const uint256 deleted = m_wallet.mapWallet.begin()->first;
        m_wallet.mapWallet.erase(deleted);
        m_wallet.NotifyTransactionChanged(deleted, CT_DELETED);
        BOOST_CHECK(!ReconcileProviderMaintenance(m_wallet, recovered, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_RECONCILING");
        if (remaining == 2) {
            BOOST_CHECK(!ReconcileProviderMaintenance(m_wallet, recovered, error));
            BOOST_CHECK_EQUAL(error, "PAYMASTER_MAINTENANCE_TRANSACTION_AMBIGUOUS");
        } else {
            BOOST_REQUIRE_MESSAGE(ReconcileProviderMaintenance(m_wallet, recovered, error), error);
            BOOST_CHECK_EQUAL(recovered, 1U);
            BOOST_REQUIRE(batch.ReadPaymasterMaintenanceLedger(ledger));
            BOOST_CHECK(ledger.records.front().transaction_id == m_wallet.mapWallet.begin()->first);
        }
    }
}

BOOST_FIXTURE_TEST_CASE(concurrent_replenishment_retains_one_current_budget_plan, WalletTestingSetup)
{
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetBroadcastTransactions(true);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        m_wallet.SetupDescriptorScriptPubKeyMans();
        ProviderPolicy offer;
        offer.funding_models = FUNDING_MODEL_SPONSORED;
        offer.min_payment = DDCents{100};
        offer.max_payment = DDCents{MAX_DD_OUTPUT_CENTS};
        offer.maximum_network_fee = DGBSatoshis{20000000};
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.WritePaymasterPolicy(offer));
    }
    ProviderLiquidityPolicy policy;
    policy.paid_maintenance_approved = true;
    policy.maximum_maintenance_fee_per_transaction = DGBSatoshis{10000000};
    policy.maximum_maintenance_fee_per_hour = DGBSatoshis{20000000};
    policy.maximum_maintenance_fee_per_day = DGBSatoshis{30000000};
    policy.updated_at = 1000;
    std::array<bool, 2> results{};
    std::array<std::string, 2> errors;
    std::array<std::thread, 2> workers;
    std::atomic<int> ready{0};
    for (size_t i = 0; i < workers.size(); ++i) {
        workers[i] = std::thread([&, i] {
            ++ready;
            while (ready.load() != 2) std::this_thread::yield();
            results[i] = RunAutomaticDGBReplenishment(m_wallet, policy, 0, 1, errors[i]);
        });
    }
    for (auto& worker : workers) worker.join();
    for (size_t i = 0; i < results.size(); ++i) {
        BOOST_CHECK(!results[i]);
        BOOST_CHECK_EQUAL(errors[i], "PAYMASTER_POOL_WAITING_DGB");
    }
    LOCK(m_wallet.cs_wallet);
    ProviderMaintenanceLedger ledger;
    BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.ReadPaymasterMaintenanceLedger(ledger));
    BOOST_REQUIRE_EQUAL(ledger.records.size(), 1U);
    BOOST_CHECK(ledger.records.front().state == ProviderMaintenanceState::PLANNED);
    BOOST_CHECK(ledger.records.front().transaction_id.IsNull());
    ProviderMaintenanceBudgetUsage usage;
    std::string error;
    BOOST_REQUIRE(SummarizeProviderMaintenanceBudget(ledger, GetTime(), usage, error));
    BOOST_CHECK_EQUAL(usage.reserved_fee.value, policy.maximum_maintenance_fee_per_transaction.value);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace
} // namespace wallet
