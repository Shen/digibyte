// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <paymaster/finance.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <script/standard.h>
#include <univalue.h>
#include <util/time.h>
#include <wallet/context.h>
#include <wallet/paymasterfinance.h>
#include <wallet/paymasterfinancecache.h>
#include <wallet/paymastercheckpoint.h>
#include <wallet/paymasteridentity.h>
#include <wallet/rpc/paymaster.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/walletdb.h>

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

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
