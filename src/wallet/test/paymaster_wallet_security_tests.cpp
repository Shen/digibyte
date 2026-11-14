// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Adversarial wallet tests for unauthorized Paymaster asset flows. */

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <coins.h>
#include <consensus/digidollar.h>
#include <digidollar/scripts.h>
#include <hash.h>
#include <index/txindex.h>
#include <key.h>
#include <key_io.h>
#include <net.h>
#include <net_processing.h>
#include <netbase.h>
#include <node/transaction.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <paymaster/directory.h>
#include <paymaster/error.h>
#include <paymaster/manager.h>
#include <paymaster/protocol.h>
#include <paymaster/provider.h>
#include <paymaster/psbt.h>
#include <paymaster/validation.h>
#include <paymaster/wire.h>
#include <script/descriptor.h>
#include <script/interpreter.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <streams.h>
#include <test/util/index.h>
#include <test/util/mining.h>
#include <txmempool.h>
#include <util/readwritefile.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/context.h>
#include <wallet/paymasteridentity.h>
#include <wallet/paymasterfinance.h>
#include <wallet/paymastercheckpoint.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterservice.h>
#include <wallet/paymasterrecoveryservice.h>
#include <wallet/paymasterstore.h>
#include <wallet/paymasterstore_internal.h>
#include <wallet/paymastersessionindex.h>
#include <wallet/rpc/paymaster_internal.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#ifdef USE_SQLITE
#include <sqlite3.h>
#include <util/translation.h>
#include <wallet/sqlite.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace wallet::paymaster_app {
ClientRecoveryResult ResolveAlternativePaymasterRecovery(WalletContext& context, CWallet& wallet,
    PaymasterStore& store, DigiDollar::Paymaster::PaymentSession& session,
    const DigiDollar::Paymaster::ProviderAttempt& original_attempt, const ClientRecoveryOptions& options);
} // namespace wallet::paymaster_app

using namespace DigiDollar::Paymaster;
using namespace wallet;
using wallet::paymaster_store::internal::MAX_PROVIDER_CAPACITY_CLEANUP_RESPONSES;

namespace {

// Scope erase-failure injection to capacity-cache atomicity tests. The shared
// mock's FailWriteAt deliberately counts WriteKey only, not EraseKey.
class CapacityEraseFailureDatabase : public MockableDatabase
{
    class Batch : public MockableBatch {
        CapacityEraseFailureDatabase& m_database;
        bool EraseKey(DataStream&& key) override
        {
            if (!m_database.m_pass) return false;
            const size_t index = m_database.erase_count++;
            if (m_database.fail_erase_at && index == *m_database.fail_erase_at) return false;
            m_database.m_records.erase(SerializeData{key.begin(), key.end()});
            return true;
        }
    public:
        explicit Batch(CapacityEraseFailureDatabase& database)
            : MockableBatch(database.m_records, database.m_pass, &database.m_on_write,
                  &database.m_refuse_write, database.m_txn_pass, &database.m_fail_write_at,
                  &database.m_write_count, &database.m_fail_commit), m_database(database) {}
    };
public:
    std::optional<size_t> fail_erase_at;
    size_t erase_count{0};
    std::unique_ptr<DatabaseBatch> MakeBatch(bool flush_on_close = true) override
    {
        return std::make_unique<Batch>(*this);
    }
};

#ifdef USE_SQLITE
uint256 SQLiteRecordDigest(WalletDatabase& database)
{
    auto batch = database.MakeBatch();
    auto cursor = batch->GetNewCursor();
    BOOST_REQUIRE(cursor);
    std::vector<std::pair<std::vector<unsigned char>, std::vector<unsigned char>>> records;
    while (true) {
        DataStream key;
        DataStream value;
        const auto status = cursor->Next(key, value);
        if (status == DatabaseCursor::Status::DONE) break;
        BOOST_REQUIRE(status == DatabaseCursor::Status::MORE);
        const auto key_bytes = MakeUCharSpan(key);
        const auto value_bytes = MakeUCharSpan(value);
        records.push_back({{key_bytes.begin(), key_bytes.end()}, {value_bytes.begin(), value_bytes.end()}});
    }
    std::sort(records.begin(), records.end());
    HashWriter hasher;
    hasher << records;
    return hasher.GetHash();
}

int SQLiteInteger(sqlite3* database, const std::string& sql)
{
    sqlite3_stmt* statement{nullptr};
    BOOST_REQUIRE_EQUAL(sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr), SQLITE_OK);
    const int result = sqlite3_step(statement);
    const int value = result == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    sqlite3_finalize(statement);
    BOOST_REQUIRE_EQUAL(result, SQLITE_ROW);
    return value;
}

/** Deny only COMMIT on one connection; rollback and reads remain available.
 * Exercises the real SQLite transaction, not a physical I/O/power-loss fault. */
class SQLiteCommitFailure {
    sqlite3* const m_database;

public:
    size_t denied_commits{0};

    explicit SQLiteCommitFailure(sqlite3* database) : m_database{database}
    {
        BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(database,
            [](void* context, int action, const char* first, const char*, const char*, const char*) {
                if (action == SQLITE_TRANSACTION && first && std::string_view{first} == "COMMIT") {
                    ++static_cast<SQLiteCommitFailure*>(context)->denied_commits;
                    return SQLITE_DENY;
                }
                return SQLITE_OK;
            }, this), SQLITE_OK);
    }

    ~SQLiteCommitFailure() { sqlite3_set_authorizer(m_database, nullptr, nullptr); }
    SQLiteCommitFailure(const SQLiteCommitFailure&) = delete;
    SQLiteCommitFailure& operator=(const SQLiteCommitFailure&) = delete;
};
#endif

template <typename T>
std::vector<unsigned char> SerializePaymasterSecurityObject(const T& object)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << object;
    const auto bytes = MakeUCharSpan(stream);
    return {bytes.begin(), bytes.end()};
}

uint256 SecurityTestId(const uint256& seed, uint16_t domain)
{
    HashWriter hasher = TaggedHash("DigiByte Paymaster Wallet Security Test v1");
    hasher << seed << domain;
    return hasher.GetSHA256();
}

uint256 SecurityCapacitySnapshotId(const PaymasterCapacityProof& proof)
{
    HashWriter hasher =
        TaggedHash("DigiByte Paymaster Capacity Snapshot v2");
    hasher << proof.version << proof.genesis_hash << proof.provider_id
           << proof.request_id << proof.session_id << proof.client_nonce
           << static_cast<uint8_t>(proof.funding_model)
           << static_cast<uint8_t>(proof.requires_carrier ? 1U : 0U);
    hasher << proof.created_at << proof.expires_at
           << static_cast<uint64_t>(proof.liquidity_slots.size());
    for (const PaymasterLiquiditySlot& slot : proof.liquidity_slots) {
        hasher << static_cast<uint8_t>(slot.carrier.has_value() ? 1U : 0U);
        if (slot.carrier) {
            hasher << slot.carrier->carrier.outpoint
                   << CTransaction{slot.carrier->carrier.creating_tx}.GetHash()
                   << slot.carrier->carrier.value
                   << slot.carrier->control_proof.reference_block
                   << slot.carrier->control_proof.expires_at;
        }
        hasher << static_cast<uint64_t>(slot.dgb_inputs.size());
        for (const CapacityDGBInput& input : slot.dgb_inputs) {
            hasher << input.input.outpoint
                   << CTransaction{input.input.creating_tx}.GetHash()
                   << input.input.value
                   << input.control_proof.reference_block
                   << input.control_proof.expires_at;
        }
    }
    return hasher.GetSHA256();
}

uint256 SecurityCapacitySessionKey(const PaymasterCapacityProof& proof)
{
    HashWriter hasher =
        TaggedHash("DigiByte Paymaster Capacity Session v1");
    hasher << proof.provider_id << proof.request_id << proof.session_id;
    return hasher.GetSHA256();
}

CScript NewTaprootScript()
{
    CKey key;
    key.MakeNewKey(true);
    TaprootBuilder builder;
    builder.Finalize(XOnlyPubKey{key.GetPubKey()});
    return GetScriptForDestination(builder.GetOutput());
}

bool AddTaprootSigningKey(wallet::CWallet& wallet,
                          const CKey& key,
                          std::string& error)
{
    // Preserve the full compressed root key when the public descriptor is
    // serialized/reloaded. A literal tr(WIF) serializes to an x-only constant;
    // its odd-Y private key is not recovered by this Core's constant-key reader.
    // An extended-key descriptor keeps the same key and exact BIP86 script.
    CExtKey extended{};
    extended.key = key;
    FlatSigningProvider provider;
    std::unique_ptr<Descriptor> descriptor = Parse(
        "tr(" + EncodeExtKey(extended) + ")", provider, error,
        /*require_checksum=*/false);
    if (!descriptor) return false;
    WalletDescriptor wallet_descriptor{
        std::move(descriptor), 0, 0, 1, 0};
    LOCK(wallet.cs_wallet);
    wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    return wallet.AddWalletDescriptor(
        wallet_descriptor, provider, "", false);
}

// Existing firewall tests intentionally inject mutually exclusive synthetic
// states for one provider. Keep the independently current marker when building
// such a fixture, so the checkpoint gate does not mask the field-specific
// invariant under test. This raw test-only injection is NOT a recovery API.
// The checkpoint_* cases deliberately do not use this helper.
void AlignSecurityFixtureCheckpoint(WalletDatabase& database)
{
    ProviderIdentityRecord identity;
    if (!WalletBatch{database}.ReadPaymasterIdentity(identity)) return;
    const auto path = PaymasterCheckpointPath(Params().GenesisBlock().GetHash(), identity.provider_id);
    FILE* file = fsbridge::fopen(path, "rb");
    BOOST_REQUIRE(file);
    std::array<unsigned char, 149> bytes{};
    const bool read = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = std::fclose(file) == 0;
    BOOST_REQUIRE(read && closed);
    CDataStream stream{bytes, SER_DISK, CLIENT_VERSION};
    uint64_t magic;
    PaymasterCheckpoint checkpoint;
    uint256 checksum;
    stream >> magic >> checkpoint >> checksum;
    BOOST_REQUIRE(checkpoint.provider_id == identity.provider_id);
    BOOST_REQUIRE(checkpoint.genesis_hash == Params().GenesisBlock().GetHash());
    BOOST_REQUIRE(checksum == (HashWriter{} << std::string{"DigiByte/PaymasterCheckpoint/v1"} << checkpoint).GetHash());
    BOOST_REQUIRE(database.MakeBatch()->Write(std::string{"pmcheckpoint"}, checkpoint));
}

std::unique_ptr<WalletDatabase> DuplicateSecurityFixtureDatabase(WalletDatabase& database)
{
    auto copy = DuplicateMockDatabase(database);
    AlignSecurityFixtureCheckpoint(*copy);
    return copy;
}

FundingSafetyLimits SecurityLimits(int64_t maximum_reserved)
{
    FundingSafetyLimits limits;
    limits.maximum_network_fee_per_transaction = DGBSatoshis{500};
    limits.maximum_reserved_network_fee = DGBSatoshis{maximum_reserved};
    limits.maximum_network_fee_per_hour = DGBSatoshis{5000};
    limits.maximum_network_fee_per_day = DGBSatoshis{50000};
    limits.maximum_completed_per_hour = 10;
    limits.maximum_completed_per_day = 100;
    return limits;
}

struct ProviderSecurityEnvironment {
    struct CapacityRecord {
        uint256 client_nonce;
        uint256 request_hash;
        uint256 session_key;
        std::vector<unsigned char> proof;
    };

    CKey identity_key;
    ProviderIdentityRecord identity;
    ProviderPolicy policy;
    ProviderSettings settings;
    ProviderSafetyPolicy safety;
    ProviderBudgetLedger ledger;
    uint256 netgroup_bucket;
    std::vector<CapacityRecord> capacity_records;
};

ProviderSecurityEnvironment MakeProviderEnvironment(int64_t now,
                                                    int64_t maximum_reserved)
{
    ProviderSecurityEnvironment environment;
    environment.identity_key.MakeNewKey(true);
    environment.identity.identity_key =
        XOnlyPubKey{environment.identity_key.GetPubKey()};
    environment.identity.provider_id =
        GetPaymasterId(environment.identity.identity_key);
    TaprootBuilder identity_builder;
    identity_builder.Finalize(environment.identity.identity_key);
    environment.identity.identity_script =
        GetScriptForDestination(identity_builder.GetOutput());
    environment.identity.display_name = "Security test provider";
    environment.identity.created_at = now - 20;

    environment.policy.funding_models = FUNDING_MODEL_SPONSORED;
    environment.policy.sponsorship_scope = SponsorshipScope::PUBLIC;
    environment.policy.fee_rate_bps = 0;
    environment.policy.min_payment = DDCents{100};
    environment.policy.max_payment = DDCents{100000};
    environment.policy.quote_ttl = 60;
    environment.policy.maximum_network_fee = DGBSatoshis{500};

    environment.settings.enabled = true;
    environment.settings.policy_hash = GetProviderPolicyHash(environment.policy);
    environment.settings.updated_at = now - 10;

    environment.safety.public_sponsored = SecurityLimits(maximum_reserved);
    environment.safety.maximum_active_quotes_total = 8;
    environment.safety.maximum_active_quotes_per_netgroup = 8;
    environment.safety.maximum_active_quotes_per_recipient = 8;
    environment.safety.maximum_quote_requests_per_netgroup_per_minute = 16;
    environment.safety.updated_at = now - 10;

    environment.ledger.recipient_bucket_secret =
        SecurityTestId(uint256S("01"), 1);
    environment.ledger.accounting_time_high_water = now - 10;
    environment.netgroup_bucket = SecurityTestId(uint256S("01"), 2);
    return environment;
}

struct ProviderQuoteFixture {
    CKey user_key;
    CTransactionRef user_transaction;
    ProviderAttempt attempt;
    ProviderPoolEntry pool_entry;
    PaymasterQuoteRequest request;
    PaymasterQuoteResponse response;
    CMutableTransaction transaction;
    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    uint256 capacity_request_hash;
};

bool BuildProviderQuoteFixture(ProviderSecurityEnvironment& environment,
                               const std::string& request_id,
                               const uint256& seed,
                               int64_t now,
                               ProviderQuoteFixture& fixture,
                               std::string& error,
                               const VerifiedDGBInput* shared_provider_input = nullptr,
                               const uint256& shared_client_nonce = {},
                               DGBSatoshis network_fee = DGBSatoshis{500},
                               CAmount transaction_output_value = 5500,
                               const uint256& genesis_hash = {},
                               int64_t intent_ttl = 30,
                               int64_t quote_ttl = 30)
{
    fixture.attempt = ProviderAttempt{};
    fixture.pool_entry = ProviderPoolEntry{};
    fixture.request = PaymasterQuoteRequest{};
    fixture.transaction = CMutableTransaction{};
    fixture.capacity_request = PaymasterCapacityRequest{};
    fixture.capacity_proof = PaymasterCapacityProof{};
    fixture.capacity_request_hash.SetNull();
    fixture.user_key.MakeNewKey(true);
    TaprootBuilder user_builder;
    user_builder.Finalize(XOnlyPubKey{fixture.user_key.GetPubKey()});
    const CScript user_script =
        GetScriptForDestination(user_builder.GetOutput());
    const CScript recipient_script = NewTaprootScript();

    CMutableTransaction user_previous;
    user_previous.vin.emplace_back(COutPoint{SecurityTestId(seed, 1), 0});
    user_previous.vout.emplace_back(4000, user_script);
    fixture.user_transaction = MakeTransactionRef(std::move(user_previous));

    VerifiedDGBInput provider_input;
    if (shared_provider_input) {
        provider_input = *shared_provider_input;
    } else {
        CMutableTransaction provider_previous;
        provider_previous.vin.emplace_back(
            COutPoint{SecurityTestId(seed, 2), 0});
        const CAmount provider_value =
            transaction_output_value + network_fee.value - 4000;
        if (provider_value <= 0) {
            error = "PAYMASTER_TEST_INVALID_PROVIDER_VALUE";
            return false;
        }
        provider_previous.vout.emplace_back(
            provider_value, environment.identity.identity_script);
        provider_input.outpoint =
            COutPoint{CTransaction{provider_previous}.GetHash(), 0};
        provider_input.creating_tx = provider_previous;
        provider_input.value = DGBSatoshis{provider_value};
    }

    if (4000 + provider_input.value.value - network_fee.value !=
        transaction_output_value) {
        error = "PAYMASTER_TEST_INVALID_TRANSACTION_BALANCE";
        return false;
    }

    fixture.transaction.vin.emplace_back(
        COutPoint{fixture.user_transaction->GetHash(), 0});
    fixture.transaction.vin.emplace_back(provider_input.outpoint);
    fixture.transaction.vout.emplace_back(transaction_output_value,
                                          recipient_script);

    CollaborativePSBTTemplate trusted;
    if (!CreateCollaborativePSBTTemplate(
            fixture.transaction,
            {{fixture.transaction.vin[0].prevout, fixture.user_transaction,
              InputRole::USER_DD},
             {fixture.transaction.vin[1].prevout,
              MakeTransactionRef(
                  CMutableTransaction{provider_input.creating_tx}),
              InputRole::PROVIDER_DGB}},
            trusted, error)) {
        return false;
    }

    PaymentIntent& intent = fixture.request.intent;
    intent.genesis_hash = genesis_hash.IsNull() ? SecurityTestId(seed, 3) : genesis_hash;
    intent.provider_id = environment.identity.provider_id;
    intent.request_id = request_id;
    intent.session_id = SecurityTestId(seed, 4);
    intent.client_nonce = shared_client_nonce.IsNull() ? SecurityTestId(seed, 5) : shared_client_nonce;
    intent.canonical_request_hash = SecurityTestId(seed, 0x105);
    intent.user_dd_inputs = {fixture.transaction.vin[0].prevout};
    intent.recipient_script = recipient_script;
    intent.recipient_amount = DDCents{1000};
    intent.user_dd_change_script = user_script;
    intent.offer_id = SecurityTestId(seed, 6);
    intent.funding_model = FundingModel::SPONSORED;
    intent.sponsorship_scope = SponsorshipScope::PUBLIC;
    intent.policy_hash = environment.settings.policy_hash;
    intent.expires_at = now + intent_ttl;
    intent.user_input_proofs = {
        {fixture.transaction.vin[0].prevout,
         std::vector<unsigned char>(64, 1)}};

    fixture.request.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    PaymasterQuote& quote = fixture.response.quote;
    quote.genesis_hash = intent.genesis_hash;
    quote.provider_id = intent.provider_id;
    quote.quote_id = SecurityTestId(seed, 7);
    quote.intent_hash = GetPaymentIntentHash(intent);
    quote.offer_id = intent.offer_id;
    quote.policy_hash = intent.policy_hash;
    quote.funding_model = intent.funding_model;
    quote.sponsorship_scope = intent.sponsorship_scope;
    quote.fee_rate_bps = 0;
    quote.service_fee = DDCents{0};
    quote.reserved_dgb_inputs = {provider_input};
    // This synthetic security fixture spends the complete native input value
    // into its single output and miner fee. Do not advertise a DGB change
    // script when the transaction contains no such output: durable commit
    // validation now treats every manifest-bound change script as authority
    // to register an exact pool successor.
    quote.dgb_change_script.reset();
    quote.network_fee = network_fee;
    quote.created_at = now;
    quote.expires_at = now + quote_ttl;
    quote.retry_until = now + 300;
    quote.unsigned_transaction = fixture.transaction;
    quote.unsigned_txid = CTransaction{fixture.transaction}.GetHash();
    quote.template_commitment =
        GetCollaborativeTemplateCommitment(intent, quote, trusted);
    quote.identity_signature.resize(64);
    if (!environment.identity_key.SignSchnorr(
            GetPaymasterQuoteSignatureHash(quote), quote.identity_signature,
            nullptr, uint256{})) {
        error = "PAYMASTER_TEST_QUOTE_SIGNING";
        return false;
    }

    fixture.response.version = DigiDollar::Paymaster::PROTOCOL_VERSION;
    fixture.response.request_id = request_id;
    fixture.response.session_id = intent.session_id;

    ProviderAttempt& attempt = fixture.attempt;
    attempt.session_id = intent.session_id;
    attempt.attempt_id = SecurityTestId(seed, 8);
    attempt.provider_id = intent.provider_id;
    attempt.provider_identity_key = environment.identity.identity_key;
    attempt.provider_endpoint = "127.0.0.1:18444";
    attempt.state = AttemptState::QUOTED;
    attempt.client_nonce = intent.client_nonce;
    attempt.intent_hash = quote.intent_hash;
    attempt.quote_id = quote.quote_id;
    attempt.unsigned_txid = quote.unsigned_txid;
    attempt.template_commitment = quote.template_commitment;
    attempt.commit_key = GetPaymasterCommitKey(
        attempt.provider_id, attempt.client_nonce, attempt.intent_hash,
        attempt.quote_id, attempt.template_commitment);
    attempt.provider_netgroup_bucket = environment.netgroup_bucket;
    attempt.unsigned_intent = SerializePaymasterSecurityObject(intent);
    attempt.quote_request =
        SerializePaymasterSecurityObject(fixture.request);
    attempt.signed_quote =
        SerializePaymasterSecurityObject(fixture.response);
    attempt.unsigned_transaction =
        SerializePaymasterSecurityObject(fixture.transaction);
    attempt.unsigned_psbt =
        SerializePaymasterSecurityObject(trusted.psbt);
    attempt.input_roles = {
        ReservationRole::USER_DD, ReservationRole::PROVIDER_DGB};
    attempt.quote_expires_at = quote.expires_at;
    attempt.retry_until = quote.retry_until;
    attempt.created_at = now;
    attempt.updated_at = now;
    if (!BuildProviderAuthorizationManifest(
            intent, quote, trusted,
            GetProviderSafetyPolicyHash(environment.safety),
            attempt.provider_manifest, error)) {
        return false;
    }

    fixture.pool_entry.outpoint = provider_input.outpoint;
    fixture.pool_entry.purpose = PoolPurpose::OPERATIONAL;
    fixture.pool_entry.asset = PoolAsset::DGB;
    fixture.pool_entry.state = PoolEntryState::RESERVED;
    fixture.pool_entry.script_pub_key =
        provider_input.creating_tx.vout[provider_input.outpoint.n].scriptPubKey;
    fixture.pool_entry.dgb_value = provider_input.value;
    fixture.pool_entry.confirmation_height = 1;
    fixture.pool_entry.reservation_id = intent.client_nonce;
    fixture.pool_entry.updated_at = now - 1;

    fixture.capacity_request.genesis_hash = intent.genesis_hash;
    fixture.capacity_request.provider_id = intent.provider_id;
    fixture.capacity_request.request_id = intent.request_id;
    fixture.capacity_request.session_id = intent.session_id;
    fixture.capacity_request.client_nonce = intent.client_nonce;
    fixture.capacity_request.funding_model = intent.funding_model;
    fixture.capacity_request.requires_carrier = false;
    fixture.capacity_request.requested_slots = 1;
    fixture.capacity_request.created_at = now - 2;
    fixture.capacity_request.expires_at = quote.expires_at;

    fixture.capacity_proof.genesis_hash = intent.genesis_hash;
    fixture.capacity_proof.provider_id = intent.provider_id;
    fixture.capacity_proof.request_id = intent.request_id;
    fixture.capacity_proof.session_id = intent.session_id;
    fixture.capacity_proof.client_nonce = intent.client_nonce;
    fixture.capacity_proof.funding_model = intent.funding_model;
    fixture.capacity_proof.requires_carrier = false;
    fixture.capacity_proof.created_at = fixture.capacity_request.created_at;
    fixture.capacity_proof.expires_at = fixture.capacity_request.expires_at;
    CapacityDGBInput capacity_input;
    capacity_input.input = provider_input;
    capacity_input.control_proof.reference_block = intent.genesis_hash;
    capacity_input.control_proof.expires_at = fixture.capacity_proof.expires_at;
    PaymasterLiquiditySlot slot;
    slot.dgb_inputs = {capacity_input};
    fixture.capacity_proof.liquidity_slots = {slot};
    fixture.capacity_proof.snapshot_id =
        SecurityCapacitySnapshotId(fixture.capacity_proof);
    CapacityDGBInput& signed_input =
        fixture.capacity_proof.liquidity_slots.front().dgb_inputs.front();
    signed_input.control_proof.signature.resize(64);
    const uint256 empty_merkle_root;
    if (!environment.identity_key.SignSchnorr(
            GetCapacityControlHash(
                fixture.capacity_proof, signed_input.input.outpoint,
                signed_input.control_proof.expires_at),
            signed_input.control_proof.signature, &empty_merkle_root,
            uint256{})) {
        error = "PAYMASTER_TEST_CAPACITY_CONTROL_SIGNING";
        return false;
    }
    fixture.capacity_proof.identity_signature.resize(64);
    if (!environment.identity_key.SignSchnorr(
            GetCapacityProofSignatureHash(fixture.capacity_proof),
            fixture.capacity_proof.identity_signature, nullptr, uint256{})) {
        error = "PAYMASTER_TEST_CAPACITY_IDENTITY_SIGNING";
        return false;
    }

    fixture.capacity_request_hash = Hash(
        SerializePaymasterSecurityObject(fixture.capacity_request));
    const uint256 request_key = GetProviderRequestSlotKey(
        intent.provider_id, intent.request_id, intent.session_id);
    if (!ReserveProviderCapacityAdmission(
            environment.ledger, environment.safety, request_key,
            fixture.capacity_request_hash, environment.netgroup_bucket,
            intent.funding_model, /*requires_carrier=*/false,
            quote.expires_at, now - 2, error)) {
        return false;
    }
    environment.capacity_records.push_back({intent.client_nonce, fixture.capacity_request_hash,
                                            SecurityCapacitySessionKey(fixture.capacity_proof),
                                            SerializePaymasterSecurityObject(fixture.capacity_proof)});
    return true;
}

bool PersistProviderQuoteEnvironment(
    wallet::CWallet& wallet,
    const ProviderSecurityEnvironment& environment,
    const std::vector<ProviderPoolEntry>& pool)
{
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    if (!(batch.WritePaymasterIdentity(environment.identity) &&
          batch.WritePaymasterPolicy(environment.policy) &&
          batch.WritePaymasterSettings(environment.settings) &&
          batch.WritePaymasterProviderSafetyPolicy(environment.safety) &&
          batch.WritePaymasterProviderBudgetLedger(environment.ledger) &&
          batch.WritePaymasterProviderPool(pool))) {
        return false;
    }
    for (const ProviderSecurityEnvironment::CapacityRecord& record :
         environment.capacity_records) {
        if (!batch.WritePaymasterCapacityNonce(
                record.client_nonce, record.request_hash) ||
            !batch.WritePaymasterCapacitySession(
                record.session_key, record.request_hash) ||
            !batch.WritePaymasterCapacityResponse(
                record.request_hash, record.proof)) {
            return false;
        }
    }
    return true;
}

bool ReadProviderSecurityState(
    wallet::CWallet& wallet,
    ProviderBudgetLedger& ledger,
    std::vector<ProviderPoolEntry>& pool)
{
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    return batch.ReadPaymasterProviderBudgetLedger(ledger) &&
           batch.ReadPaymasterProviderPool(pool);
}

struct PaymasterMempoolTestingSetup : public WalletTestingSetup {
    PaymasterMempoolTestingSetup()
        : WalletTestingSetup{ChainType::REGTEST}
    {
        BOOST_REQUIRE(!g_txindex);
        auto txindex = std::make_unique<TxIndex>(
            interfaces::MakeChain(m_node), 1 << 20,
            /*f_memory=*/true);
        BOOST_REQUIRE(txindex->Init());
        BOOST_REQUIRE(txindex->StartBackgroundSync());
        g_txindex = std::move(txindex);
        IndexWaitSynced(*g_txindex);
    }

    ~PaymasterMempoolTestingSetup()
    {
        SyncWithValidationInterfaceQueue();
        g_txindex->Stop();
        g_txindex.reset();
    }
};

const ProviderBudgetReservation* FindBudgetReservation(
    const ProviderBudgetLedger& ledger,
    const uint256& commit_key)
{
    const auto it = std::find_if(
        ledger.reservations.begin(), ledger.reservations.end(),
        [&](const ProviderBudgetReservation& reservation) {
            return reservation.commit_key == commit_key;
        });
    return it == ledger.reservations.end() ? nullptr : &*it;
}

const ProviderCapacityAdmission* FindCapacityAdmission(
    const ProviderBudgetLedger& ledger,
    const uint256& commit_key)
{
    const auto it = std::find_if(
        ledger.capacity_admissions.begin(), ledger.capacity_admissions.end(),
        [&](const ProviderCapacityAdmission& admission) {
            return admission.commit_key == commit_key;
        });
    return it == ledger.capacity_admissions.end() ? nullptr : &*it;
}

bool BuildDurableClientFinalFixture(
    wallet::CWallet& wallet,
    ProviderSecurityEnvironment& environment,
    ProviderQuoteFixture& fixture,
    int64_t now,
    PaymentSession& session,
    PaymasterResult& result,
    CMutableTransaction& final_transaction,
    std::string& error)
{
    if (!AddTaprootSigningKey(wallet, fixture.user_key, error) ||
        !AddTaprootSigningKey(wallet, environment.identity_key, error)) {
        return false;
    }
    const CTransactionRef provider_transaction = MakeTransactionRef(
        CMutableTransaction{
            fixture.response.quote.reserved_dgb_inputs.front().creating_tx});
    {
        LOCK(wallet.cs_wallet);
        if (!wallet.AddToWallet(fixture.user_transaction,
                                TxStateInMempool{}) ||
            !wallet.AddToWallet(provider_transaction,
                                TxStateInMempool{})) {
            error = "PAYMASTER_TEST_WALLET_PREVOUT_INSERT";
            return false;
        }
    }

    PaymasterCapacityRequest capacity_request;
    capacity_request.genesis_hash = fixture.request.intent.genesis_hash;
    capacity_request.provider_id = fixture.attempt.provider_id;
    capacity_request.request_id = fixture.request.intent.request_id;
    capacity_request.session_id = fixture.attempt.session_id;
    capacity_request.client_nonce = fixture.attempt.client_nonce;
    capacity_request.funding_model = fixture.request.intent.funding_model;
    capacity_request.requires_carrier = false;
    capacity_request.requested_slots = 1;
    capacity_request.created_at = now - 2;
    capacity_request.expires_at = fixture.response.quote.expires_at;

    PaymasterCapacityProof capacity_proof;
    capacity_proof.genesis_hash = capacity_request.genesis_hash;
    capacity_proof.provider_id = capacity_request.provider_id;
    capacity_proof.request_id = capacity_request.request_id;
    capacity_proof.session_id = capacity_request.session_id;
    capacity_proof.client_nonce = capacity_request.client_nonce;
    capacity_proof.funding_model = capacity_request.funding_model;
    capacity_proof.requires_carrier = capacity_request.requires_carrier;
    capacity_proof.created_at = capacity_request.created_at;
    capacity_proof.expires_at = capacity_request.expires_at;
    CapacityDGBInput capacity_input;
    capacity_input.input =
        fixture.response.quote.reserved_dgb_inputs.front();
    capacity_input.control_proof.reference_block =
        Params().GenesisBlock().GetHash();
    capacity_input.control_proof.expires_at = capacity_proof.expires_at;
    PaymasterLiquiditySlot capacity_slot;
    capacity_slot.dgb_inputs = {capacity_input};
    capacity_proof.liquidity_slots = {capacity_slot};
    capacity_proof.snapshot_id =
        SecurityCapacitySnapshotId(capacity_proof);
    CapacityDGBInput& signed_capacity_input =
        capacity_proof.liquidity_slots.front().dgb_inputs.front();
    signed_capacity_input.control_proof.signature.resize(64);
    const uint256 empty_merkle_root;
    if (!environment.identity_key.SignSchnorr(
            GetCapacityControlHash(
                capacity_proof, signed_capacity_input.input.outpoint,
                signed_capacity_input.control_proof.expires_at),
            signed_capacity_input.control_proof.signature,
            &empty_merkle_root, uint256{})) {
        error = "PAYMASTER_TEST_CAPACITY_CONTROL_SIGNING";
        return false;
    }
    capacity_proof.identity_signature.resize(64);
    if (!environment.identity_key.SignSchnorr(
            GetCapacityProofSignatureHash(capacity_proof),
            capacity_proof.identity_signature, nullptr, uint256{})) {
        error = "PAYMASTER_TEST_CAPACITY_IDENTITY_SIGNING";
        return false;
    }

    fixture.attempt.capacity_request =
        SerializePaymasterSecurityObject(capacity_request);
    fixture.attempt.capacity_snapshot.snapshot_id =
        capacity_proof.snapshot_id;
    fixture.attempt.capacity_snapshot.resource_commitment =
        GetCapacityResourceCommitment(capacity_proof);
    fixture.attempt.capacity_snapshot.session_id =
        fixture.attempt.session_id;
    fixture.attempt.capacity_snapshot.attempt_id =
        fixture.attempt.attempt_id;
    fixture.attempt.capacity_snapshot.provider_id =
        fixture.attempt.provider_id;
    fixture.attempt.capacity_snapshot.client_nonce =
        fixture.attempt.client_nonce;
    fixture.attempt.capacity_snapshot.request_hash =
        Hash(fixture.attempt.capacity_request);
    fixture.attempt.capacity_snapshot.capacity_proof =
        SerializePaymasterSecurityObject(capacity_proof);
    fixture.attempt.capacity_snapshot.created_at =
        capacity_proof.created_at;
    fixture.attempt.capacity_snapshot.expires_at =
        capacity_proof.expires_at;
    fixture.attempt.capacity_snapshot.validated_at = now - 1;
    fixture.attempt.capacity_snapshot.funding_model =
        capacity_proof.funding_model;
    fixture.attempt.capacity_snapshot.requires_carrier =
        capacity_proof.requires_carrier;

    CollaborativePSBTTemplate trusted;
    {
        SpanReader stream{::PROTOCOL_VERSION,
                          fixture.attempt.unsigned_psbt};
        stream >> trusted.psbt;
        if (!stream.empty()) {
            error = "PAYMASTER_TEST_PSBT_ENCODING";
            return false;
        }
    }
    trusted.input_roles = {
        InputRole::USER_DD, InputRole::PROVIDER_DGB};
    if (!BuildClientAuthorizationManifest(
            fixture.request.intent, fixture.response.quote,
            fixture.attempt.capacity_snapshot, trusted,
            DDCents{0}, fixture.attempt.client_manifest, error)) {
        return false;
    }
    fixture.attempt.accepted_client_manifest_id =
        fixture.attempt.client_manifest.manifest_id;
    fixture.attempt.client_manifest_accepted_at = now;

    PartiallySignedTransaction signed_psbt{trusted.psbt};
    if (!SignCollaborativePSBTForParty(
            wallet, signed_psbt, trusted, SigningParty::USER, error)) {
        return false;
    }
    fixture.attempt.user_signed_psbt =
        SerializePaymasterSecurityObject(signed_psbt);
    if (!SignCollaborativePSBTForParty(
            wallet, signed_psbt, trusted, SigningParty::PROVIDER, error) ||
        !FinalizeAndExtractPSBT(signed_psbt, final_transaction)) {
        if (error.empty()) error = "PAYMASTER_TEST_FINALIZATION";
        return false;
    }
    const CTransaction final{final_transaction};
    fixture.attempt.state = AttemptState::FINAL_COMMITTED;
    fixture.attempt.final_transaction =
        SerializePaymasterSecurityObject(final_transaction);
    fixture.attempt.final_txid = final.GetHash();
    fixture.attempt.updated_at = now + 1;

    session.request_id = fixture.request.intent.request_id;
    session.session_id = fixture.attempt.session_id;
    session.canonical_request_hash =
        fixture.request.intent.canonical_request_hash;
    session.fee_mode_requested = FeeMode::PAYMASTER;
    session.fee_mode_used = FeeMode::PAYMASTER;
    session.state = SessionState::PENDING_PROVIDER;
    session.pending_phase = PendingPhase::PROVIDER_SIGNED_KNOWN;
    session.user_inputs = fixture.request.intent.user_dd_inputs;
    session.attempt_ids = {fixture.attempt.attempt_id};
    session.created_at = now - 1;
    session.updated_at = now + 1;
    session.final_txid = final.GetHash();

    result.genesis_hash = fixture.request.intent.genesis_hash;
    result.provider_id = fixture.attempt.provider_id;
    result.commit_key = fixture.attempt.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::FINAL_COMMITTED;
    result.txid = final.GetHash();
    result.raw_transaction_hash = final.GetWitnessHash();
    result.final_transaction = final_transaction;
    result.updated_at = now + 1;
    result.identity_signature.resize(64);
    if (!environment.identity_key.SignSchnorr(
            GetPaymasterResultSignatureHash(result),
            result.identity_signature, nullptr, uint256{})) {
        error = "PAYMASTER_TEST_RESULT_SIGNING";
        return false;
    }
    error.clear();
    return true;
}

// Quote retries still pass the real readiness gates. Reuse one setup for the
// classification matrix and the actual automatic service cycle below.
struct QuoteRequestProcessingSetup : TestChain100Setup {
    wallet::CWallet wallet{m_node.chain.get(), "quote-classification", CreateMockableWalletDatabase()};
    ProviderSecurityEnvironment environment{MakeProviderEnvironment(GetTime(), 2000)};
    ProviderQuoteFixture fixture;
    WalletContext context;
    Manager manager{true};
    const uint256 seed{uint256S("1821")};
    int64_t now{0};

    struct ServicesCleanup {
        CConnman& connman;
        ~ServicesCleanup()
        {
            connman.Interrupt();
            connman.Stop();
            if (g_txindex) g_txindex->Interrupt();
            SyncWithValidationInterfaceQueue();
            if (g_txindex) g_txindex->Stop();
            g_txindex.reset();
        }
    } services_cleanup{*m_node.connman};

    explicit QuoteRequestProcessingSetup(int outbound_connections = 0, DGBSatoshis network_fee = DGBSatoshis{500})
    {
        BOOST_REQUIRE(wallet.LoadWallet() == DBErrors::LOAD_OK);
        environment.policy.maximum_network_fee = DGBSatoshis{COIN / 10};
        environment.settings.policy_hash = GetProviderPolicyHash(environment.policy);
        auto& limits = environment.safety.public_sponsored;
        limits.maximum_network_fee_per_transaction = DGBSatoshis{COIN / 10};
        limits.maximum_reserved_network_fee = DGBSatoshis{COIN};
        limits.maximum_network_fee_per_hour = DGBSatoshis{COIN};
        limits.maximum_network_fee_per_day = DGBSatoshis{COIN};
        environment.settings.operation_mode = ProviderOperationMode::MANUAL;
        std::string error;
        BOOST_REQUIRE_MESSAGE(AddTaprootSigningKey(wallet, environment.identity_key, error), error);
        std::vector<ProviderPoolEntry> pool;
        for (size_t i = 0; i < REQUIRED_ADMISSION_SLOTS; ++i) {
            const CBlock block = CreateAndProcessBlock({}, environment.identity.identity_script);
            const int height = *m_node.chain->getHeight();
            {
                LOCK(wallet.cs_wallet);
                BOOST_REQUIRE(wallet.AddToWallet(block.vtx.front(), TxStateConfirmed{block.GetHash(), height, 0}));
            }
            ProviderPoolEntry entry;
            entry.outpoint = COutPoint{block.vtx.front()->GetHash(), 0};
            entry.purpose = PoolPurpose::ADMISSION;
            entry.asset = PoolAsset::DGB;
            entry.state = PoolEntryState::AVAILABLE;
            entry.script_pub_key = block.vtx.front()->vout.front().scriptPubKey;
            entry.dgb_value = DGBSatoshis{block.vtx.front()->vout.front().nValue};
            entry.confirmation_height = height;
            entry.updated_at = GetTime();
            pool.push_back(entry);
            SetMockTime(GetTime() + 1);
        }
        now = GetTime();
        // A shorter signed quote lifetime lets expiry reach the replay branch
        // while the original, byte-identical intent envelope is still valid.
        BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(environment,
            "550e8400-e29b-41d4-a716-446655441098", seed, now, fixture, error,
            nullptr, {}, network_fee, 5500, Params().GenesisBlock().GetHash(),
            /*intent_ttl=*/60, /*quote_ttl=*/20), error);
        fixture.pool_entry.confirmation_height = 0;
        pool.push_back(fixture.pool_entry);
        BOOST_REQUIRE(PersistProviderQuoteEnvironment(wallet, environment, pool));
        PaymasterStore store{wallet};
        BOOST_REQUIRE_MESSAGE(store.CommitProviderQuote(
            fixture.attempt, fixture.request.intent.genesis_hash, now, error), error);
        {
            LOCK(wallet.cs_wallet);
            wallet.SetLastBlockProcessed(*m_node.chain->getHeight(),
                m_node.chain->getBlockHash(*m_node.chain->getHeight()));
        }

        BOOST_REQUIRE(!g_txindex);
        g_txindex = std::make_unique<TxIndex>(interfaces::MakeChain(m_node), 1 << 20, true);
        BOOST_REQUIRE(g_txindex->Init());
        BOOST_REQUIRE(g_txindex->StartBackgroundSync());
        IndexWaitSynced(*g_txindex);
        m_node.args->ForceSetArg("-txindex", "1");
        m_node.args->ForceSetArg("-paymasterendpoint", "127.0.0.1:18445");
        m_node.args->ForceSetArg("-dnsseed", "0");
        m_node.args->ForceSetArg("-dandelion", "0");
        CConnman::Options options{};
        options.nLocalServices = ServiceFlags{NODE_NETWORK | NODE_WITNESS | NODE_P2P_V2};
        options.nMaxConnections = 8;
        options.nMaxAddnode = 1;
        options.paymaster_inbound = 2;
        options.paymaster_outbound = outbound_connections;
        options.m_msgproc = m_node.peerman.get();
        options.m_use_addrman_outgoing = false;
        options.vBinds = {LookupNumeric("127.0.0.1:0")};
        options.paymaster_binds = {{LookupNumeric("127.0.0.1:0"), false}};
        BOOST_REQUIRE(m_node.connman->Start(*m_node.scheduler, options));

        context.chain = m_node.chain.get();
        context.args = m_node.args;
        context.paymaster = &manager;
        BOOST_REQUIRE(manager.StartProvider(wallet.GetName(), environment.identity.provider_id));
        const auto readiness = paymaster_app::GetProviderReadiness(wallet, context);
        BOOST_REQUIRE_EQUAL(readiness.errors.size(), 1U);
        BOOST_REQUIRE_EQUAL(readiness.errors.front(), "PAYMASTER_OPERATIONAL_SLOT_MISSING");
        BOOST_REQUIRE(paymaster_app::CapacityContinuationMayProceed(readiness));
        BOOST_REQUIRE(paymaster_app::ProviderTxIndexIsReady(wallet, false));
    }

    uint256 EnqueueQuote(Manager& transport, const PaymasterQuoteRequest& request)
    {
        std::string error;
        BOOST_REQUIRE_MESSAGE(ValidateQuoteRequestEnvelope(request,
            Params().GenesisBlock().GetHash(), GetTime(), error), error);
        const auto bytes = SerializePaymasterSecurityObject(request);
        const uint256 message_id{Hash(bytes)};
        // This is the normal network admission path after v2 channel and
        // envelope validation, with no fabricated capacity exchange.
        BOOST_REQUIRE(transport.CanReceiveDirectMessage(44, DirectPayload{request},
            DirectAdmissionClass::REQUEST, GetTime()));
        BOOST_REQUIRE(transport.EnqueueNetworkDirectMessage(44, message_id, bytes.size(),
            DirectPayload{request}, GetTime(), 44, {1, 8, 8},
            DirectAdmissionClass::REQUEST) == DirectEnqueueResult::ACCEPTED);
        return message_id;
    }

    PaymasterQuoteRequest ConflictingRequest() const
    {
        auto request = fixture.request;
        request.intent.session_id = SecurityTestId(seed, 30);
        return request;
    }

    std::string CheckpointBytes() const
    {
        const auto [read, bytes] = ReadBinaryFile(PaymasterCheckpointPath(
            Params().GenesisBlock().GetHash(), environment.identity.provider_id));
        BOOST_REQUIRE(read);
        return bytes;
    }

    void CheckClassification(const PaymasterQuoteRequest& request,
                             bool local_failure, const std::string& reason)
    {
        Manager transport{true};
        context.paymaster = &transport;
        BOOST_REQUIRE(transport.StartProvider(wallet.GetName(), environment.identity.provider_id));
        EnqueueQuote(transport, request);
        const auto before = GetMockableDatabase(wallet).m_records;
        const auto checkpoint_before = CheckpointBytes();
        try {
            const auto result = paymaster_app::ProcessPaymasterRequests(
                context, wallet, paymaster_app::ExecutionMode::MANUAL);
            BOOST_CHECK(!local_failure);
            BOOST_CHECK(result.HasMessage());
            BOOST_CHECK(result.IsRejected());
            BOOST_CHECK(!result.queued.value_or(false));
            BOOST_CHECK_EQUAL(result.message_type.value_or(""), "quote");
            BOOST_CHECK_EQUAL(result.rejection_reason.value_or(""), reason);
        } catch (const ApplicationError& failure) {
            BOOST_CHECK_MESSAGE(local_failure, failure.what());
            BOOST_CHECK(failure.kind == ErrorKind::WALLET);
            BOOST_CHECK_EQUAL(failure.what(), reason);
        }
        BOOST_CHECK_EQUAL(transport.DirectMessageCount(), local_failure ? 1U : 0U);
        BOOST_CHECK_EQUAL(transport.OutboundDirectMessageCount(), 0U);
        BOOST_CHECK(transport.IsProviderRunning(wallet.GetName(), environment.identity.provider_id));
        BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);
        BOOST_CHECK(CheckpointBytes() == checkpoint_before);
        context.paymaster = &manager;
    }
};

struct ClientRecoveryReadSetup : QuoteRequestProcessingSetup {
    ClientRecoveryReadSetup() : QuoteRequestProcessingSetup{1} {}
};

AlternativeRecoveryRequest MakeRecoveryReadRequest(const ProviderSecurityEnvironment& environment,
                                                   const uint256& seed, int64_t now)
{
    AlternativeRecoveryRequest request;
    request.genesis_hash = Params().GenesisBlock().GetHash();
    request.request_id = "550e8400-e29b-41d4-a716-446655441099";
    request.session_id = SecurityTestId(seed, 101);
    request.original_provider_id = SecurityTestId(seed, 102);
    request.recovery_provider_id = environment.identity.provider_id;
    request.offer_id = SecurityTestId(seed, 103);
    request.policy_hash = SecurityTestId(seed, 104);
    request.original_commit_key = SecurityTestId(seed, 105);
    request.original_template_commitment = SecurityTestId(seed, 106);
    request.client_nonce = SecurityTestId(seed, 107);
    request.capacity_snapshot_id = SecurityTestId(seed, 108);
    request.capacity_resource_commitment = SecurityTestId(seed, 109);
    request.capacity_request.genesis_hash = request.genesis_hash;
    request.capacity_request.provider_id = request.recovery_provider_id;
    request.capacity_request.request_id = request.request_id;
    request.capacity_request.session_id = request.session_id;
    request.capacity_request.client_nonce = request.client_nonce;
    request.capacity_request.funding_model = FundingModel::USER_PAID;
    request.capacity_request.requested_slots = 1;
    request.capacity_request.created_at = now;
    request.capacity_request.expires_at = now + 30;
    request.user_dd_inputs = {COutPoint{SecurityTestId(seed, 110), 0}};
    request.user_input_proofs = {{request.user_dd_inputs.front(), std::vector<unsigned char>(64, 1)}};
    request.wallet_returns = {{NewTaprootScript(), DDCents{900}}};
    request.maximum_service_fee = request.service_fee = DDCents{100};
    request.created_at = now;
    request.expires_at = now + 25;
    return request;
}

AlternativeRecoveryRecord MakeRecoveryReadReplay(const ProviderSecurityEnvironment& environment,
                                                 const AlternativeRecoveryRequest& request, int64_t now)
{
    AlternativeRecoveryRecord record;
    record.provider_side = true;
    record.request_id = request.request_id;
    record.session_id = request.session_id;
    record.original_provider_id = request.original_provider_id;
    record.recovery_provider_id = request.recovery_provider_id;
    record.recovery_provider_identity_key = environment.identity.identity_key;
    record.recovery_provider_endpoint = "127.0.0.1:18445";
    record.offer_id = request.offer_id;
    record.policy_hash = request.policy_hash;
    record.original_commit_key = request.original_commit_key;
    record.original_template_commitment = request.original_template_commitment;
    record.client_nonce = request.client_nonce;
    record.capacity_request = request.capacity_request;
    record.recovery_request = request;
    record.recovery_id = GetAlternativeRecoveryId(request.request_id, request.session_id,
        request.recovery_provider_id, request.client_nonce);
    record.recovery_request_hash = GetAlternativeRecoveryRequestHash(request);
    record.phase = AlternativeRecoveryPhase::RESPONSE_VALIDATED;
    record.created_at = record.updated_at = now;
    auto& response = record.recovery_response;
    response.genesis_hash = request.genesis_hash;
    response.request_id = request.request_id;
    response.session_id = request.session_id;
    response.recovery_id = record.recovery_id;
    response.recovery_provider_id = record.recovery_provider_id;
    response.recovery_request_hash = record.recovery_request_hash;
    response.manifest.request_id = request.request_id;
    response.manifest.session_id = request.session_id;
    response.manifest.original_provider_id = request.original_provider_id;
    response.manifest.recovery_provider_id = request.recovery_provider_id;
    response.manifest.offer_id = request.offer_id;
    response.manifest.policy_hash = request.policy_hash;
    response.manifest.original_commit_key = request.original_commit_key;
    response.manifest.original_template_commitment = request.original_template_commitment;
    response.manifest.capacity_snapshot_id = request.capacity_snapshot_id;
    response.manifest.capacity_resource_commitment = request.capacity_resource_commitment;
    response.manifest.user_dd_inputs = request.user_dd_inputs;
    response.manifest.wallet_returns = request.wallet_returns;
    response.manifest.maximum_service_fee = request.maximum_service_fee;
    response.manifest.service_fee = request.service_fee;
    response.manifest.network_fee = DGBSatoshis{500};
    response.manifest.unsigned_txid = request.original_commit_key;
    response.manifest.template_commitment = request.original_template_commitment;
    response.manifest.expires_at = now + 20;
    response.manifest.manifest_id = GetAlternativeRecoveryManifestId(response.manifest);
    response.unsigned_psbt = {1};
    response.created_at = now;
    response.expires_at = now + 20;
    response.recovery_commit_key = GetAlternativeRecoveryCommitKey(response);
    response.identity_signature.resize(64);
    BOOST_REQUIRE(environment.identity_key.SignSchnorr(GetAlternativeRecoveryResponseSignatureHash(response),
        response.identity_signature, nullptr, uint256{}));
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoveryResponse(response, request,
        environment.identity.identity_key, now, error), error);
    return record;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_wallet_security_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(test_signing_keys_survive_encrypted_reload_with_both_parities)
{
    for (const unsigned char parity : {0x02, 0x03}) {
        BOOST_TEST_CONTEXT("compressed public-key prefix " << int{parity}) {
            CKey key;
            do { key.MakeNewKey(true); } while (key.GetPubKey()[0] != parity);
            TaprootBuilder builder;
            builder.Finalize(XOnlyPubKey{key.GetPubKey()});
            const CScript script = GetScriptForDestination(builder.GetOutput());
            wallet::CWallet original{m_node.chain.get(), "encrypted-fixture-key", CreateMockableWalletDatabase()};
            BOOST_REQUIRE(original.LoadWallet() == DBErrors::LOAD_OK);
            std::string error;
            BOOST_REQUIRE_MESSAGE(AddTaprootSigningKey(original, key, error), error);
            const auto has_key = [&](wallet::CWallet& wallet) {
                LOCK(wallet.cs_wallet);
                for (auto* manager : wallet.GetScriptPubKeyMans(script)) {
                    auto* descriptor = dynamic_cast<DescriptorScriptPubKeyMan*>(manager);
                    if (!descriptor) continue;
                    auto provider = descriptor->GetSigningProviderWithKeys(script);
                    CKey restored;
                    if (provider && provider->GetKeyByXOnly(XOnlyPubKey{key.GetPubKey()}, restored) && restored == key) return true;
                }
                return false;
            };
            BOOST_REQUIRE(has_key(original));
            SecureString passphrase{"fixture-parity-reload"};
            BOOST_REQUIRE(original.EncryptWallet(passphrase));
            BOOST_CHECK(!has_key(original));
            wallet::CWallet restored{m_node.chain.get(), "reloaded-fixture-key",
                                     DuplicateMockDatabase(original.GetDatabase())};
            BOOST_REQUIRE(restored.LoadWallet() == DBErrors::LOAD_OK);
            BOOST_CHECK(restored.IsLocked());
            BOOST_CHECK(!has_key(restored));
            BOOST_REQUIRE(restored.Unlock(passphrase));
            BOOST_CHECK(has_key(restored));
        }
    }
}

BOOST_AUTO_TEST_CASE(checkpoint_transaction_atomicity_and_unmarked_rollback)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    PaymasterCheckpoint before;
    BOOST_REQUIRE(database.MakeBatch()->Read(std::string{"pmcheckpoint"}, before));
    const auto old_image = database.m_records;
    {
        WalletBatch batch{database};
        BOOST_REQUIRE(batch.TxnBegin());
        BOOST_REQUIRE(batch.WritePaymasterSettings(environment.settings));
        BOOST_REQUIRE(batch.WritePaymasterPolicy(environment.policy));
        BOOST_REQUIRE(batch.TxnCommit());
    }
    PaymasterCheckpoint after;
    BOOST_REQUIRE(database.MakeBatch()->Read(std::string{"pmcheckpoint"}, after));
    BOOST_CHECK_EQUAL(after.generation, before.generation + 1);
    BOOST_CHECK(after.token != before.token);
    std::string error;
    BOOST_REQUIRE_MESSAGE(CheckPaymasterCheckpoint(database, error), error);
    wallet::CWallet copied{m_node.chain.get(), "renamed-unmarked-backup", std::make_unique<MockableDatabase>(old_image)};
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(copied, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED");
    BOOST_CHECK(!WalletBatch{copied.GetDatabase()}.WritePaymasterPolicy(environment.policy));
    BOOST_CHECK(GetMockableDatabase(copied).m_records == old_image);
    // Abandon an uncommitted transaction. Its counter and lock must disappear
    // with the batch, without advancing the independently retained file.
    const auto current_image = database.m_records;
    {
        WalletBatch abandoned{database};
        BOOST_REQUIRE(abandoned.TxnBegin());
        BOOST_REQUIRE(abandoned.WritePaymasterSettings(environment.settings));
    }
    BOOST_CHECK(database.m_records == current_image);
    BOOST_REQUIRE_MESSAGE(CheckPaymasterCheckpoint(database, error), error);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterSettings(environment.settings));
}

BOOST_AUTO_TEST_CASE(checkpoint_matching_pair_rollback_cannot_detect_newer_history)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    const auto old_image = database.m_records;
    const auto path = PaymasterCheckpointPath(Params().GenesisBlock().GetHash(), environment.identity.provider_id);
    const auto snapshot = m_path_root / "matching-checkpoint.bak";
    BOOST_REQUIRE(fs::copy_file(path, snapshot, fs::copy_options::none));
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterSettings(environment.settings));
    wallet::CWallet copied{m_node.chain.get(), "whole-pair-rollback", std::make_unique<MockableDatabase>(old_image)};
    std::string error;
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(copied, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED");
    // Only this disposable fixture deliberately rolls back both independent
    // stores. A matching pair contains no evidence of the later generation;
    // this passing test records a limitation, not protection against it.
    BOOST_REQUIRE(fs::copy_file(snapshot, path, fs::copy_options::overwrite_existing));
    BOOST_REQUIRE_MESSAGE(CheckPaymasterProviderRestoreGuard(copied, error), error);
    BOOST_REQUIRE(WalletBatch{copied.GetDatabase()}.WritePaymasterSettings(environment.settings));
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(m_wallet, error));
    // The resulting new branch fences the other image again at this store.
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED");
}

BOOST_AUTO_TEST_CASE(checkpoint_concurrent_copies_cannot_both_commit)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    wallet::CWallet clone{m_node.chain.get(), "parallel-clone", std::make_unique<MockableDatabase>(database.m_records)};
    const auto clone_before = GetMockableDatabase(clone).m_records;
    WalletBatch first{database};
    BOOST_REQUIRE(first.TxnBegin());
    BOOST_REQUIRE(first.WritePaymasterSettings(environment.settings));
    // Before the first commit the independent generation is still current for
    // both images. The live transaction lock must protect this exact window.
    bool second_committed{true};
    std::thread second([&] {
        second_committed = WalletBatch{clone.GetDatabase()}.WritePaymasterSettings(environment.settings);
    });
    second.join();
    BOOST_CHECK(!second_committed);
    BOOST_CHECK(GetMockableDatabase(clone).m_records == clone_before);
    BOOST_REQUIRE(first.TxnCommit());
    std::string error;
    BOOST_REQUIRE_MESSAGE(CheckPaymasterCheckpoint(database, error), error);
    BOOST_CHECK(!CheckPaymasterCheckpoint(clone.GetDatabase(), error));
}

BOOST_AUTO_TEST_CASE(checkpoint_failed_commit_blocks_retry_and_restart)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    const auto before = database.m_records;
    database.m_fail_commit = true;
    BOOST_CHECK(!WalletBatch{database}.WritePaymasterSettings(environment.settings));
    database.m_fail_commit = false;
    BOOST_CHECK(database.m_records == before);
    std::string error;
    BOOST_CHECK(!CheckPaymasterCheckpoint(database, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED");
    BOOST_CHECK(!WalletBatch{database}.WritePaymasterSettings(environment.settings));
    wallet::CWallet restarted{m_node.chain.get(), "commit-failed-restart", std::make_unique<MockableDatabase>(before)};
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(restarted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED");
}

BOOST_AUTO_TEST_CASE(checkpoint_missing_corrupt_and_unmarked_records_fail_closed)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    const auto path = PaymasterCheckpointPath(Params().GenesisBlock().GetHash(), environment.identity.provider_id);
    const auto saved = m_path_root / "checkpoint-valid.bak";
    BOOST_REQUIRE(fs::copy_file(path, saved, fs::copy_options::none));
    std::string error;
    FILE* file = fsbridge::fopen(path, "ab");
    BOOST_REQUIRE(file);
    BOOST_REQUIRE(std::fputc(0, file) != EOF);
    BOOST_REQUIRE_EQUAL(std::fclose(file), 0);
    BOOST_CHECK(!CheckPaymasterCheckpoint(database, error));
    BOOST_CHECK(!WalletBatch{database}.WritePaymasterSettings(environment.settings));
    BOOST_REQUIRE(fs::remove(path));
    BOOST_CHECK(!CheckPaymasterCheckpoint(database, error));
    BOOST_CHECK(!WalletBatch{database}.WritePaymasterSettings(environment.settings));
    BOOST_REQUIRE(fs::copy_file(saved, path, fs::copy_options::none));
    BOOST_REQUIRE_MESSAGE(CheckPaymasterCheckpoint(database, error), error);
    BOOST_REQUIRE(database.MakeBatch()->Erase(std::string{"pmcheckpoint"}));
    // Losing the in-wallet marker does not permit re-enrollment when the
    // external provider checkpoint already exists.
    BOOST_CHECK(!CheckPaymasterCheckpoint(database, error));
    BOOST_CHECK(!WalletBatch{database}.WritePaymasterSettings(environment.settings));
}

BOOST_AUTO_TEST_CASE(checkpoint_matching_invalid_pair_and_exhaustion_fail_closed)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    PaymasterCheckpoint original;
    BOOST_REQUIRE(database.MakeBatch()->Read(std::string{"pmcheckpoint"}, original));
    const auto path = PaymasterCheckpointPath(original.genesis_hash, original.provider_id);
    for (int mutation = 0; mutation < 8; ++mutation) {
        auto changed = original;
        if (mutation == 0) ++changed.version;
        if (mutation == 1) changed.genesis_hash = uint256S("1");
        if (mutation == 2) changed.provider_id = uint256S("2");
        if (mutation == 3) changed.generation = 0;
        if (mutation == 4) changed.token.SetNull();
        if (mutation == 5) changed.pending = 3;
        if (mutation == 6) changed.generation = (std::numeric_limits<uint64_t>::max)();
        if (mutation == 7) changed.pending = 1;
        // Both stores agree and the checksum is valid: rejection must come
        // from semantic binding/version/overflow/pending checks, not mismatch.
        CDataStream bytes{SER_DISK, CLIENT_VERSION};
        bytes << uint64_t{0x3150435042474444} << changed
              << (HashWriter{} << std::string{"DigiByte/PaymasterCheckpoint/v1"} << changed).GetHash();
        FILE* file = fsbridge::fopen(path, "wb");
        BOOST_REQUIRE(file);
        BOOST_REQUIRE_EQUAL(std::fwrite(bytes.data(), 1, bytes.size(), file), bytes.size());
        BOOST_REQUIRE_EQUAL(std::fclose(file), 0);
        BOOST_REQUIRE(database.MakeBatch()->Write(std::string{"pmcheckpoint"}, changed));
        std::string error;
        BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(m_wallet, error));
        BOOST_CHECK(!WalletBatch{database}.WritePaymasterSettings(environment.settings));
        PaymasterCheckpoint retained;
        BOOST_REQUIRE(database.MakeBatch()->Read(std::string{"pmcheckpoint"}, retained));
        BOOST_CHECK(retained == changed);
    }
}

BOOST_AUTO_TEST_CASE(checkpoint_pending_native_operation_survives_restart)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    std::string error;
    {
        LOCK(m_wallet.cs_wallet);
        PaymasterCheckpointOperation operation{m_wallet};
        BOOST_REQUIRE_MESSAGE(operation.Begin(error), error);
        BOOST_REQUIRE_MESSAGE(CheckPaymasterCheckpoint(database, error), error);
        BOOST_REQUIRE(WalletBatch{database}.WritePaymasterSettings(environment.settings));
        BOOST_REQUIRE_MESSAGE(operation.Complete(error), error);
    }
    BOOST_REQUIRE_MESSAGE(CheckPaymasterCheckpoint(database, error), error);
    {
        LOCK(m_wallet.cs_wallet);
        PaymasterCheckpointOperation interrupted{m_wallet};
        BOOST_REQUIRE_MESSAGE(interrupted.Begin(error), error);
        // A second thread cannot use the first operation's temporary authority.
        bool accepted{true};
        std::thread other([&] {
            std::string thread_error;
            accepted = CheckPaymasterCheckpoint(database, thread_error);
        });
        other.join();
        BOOST_CHECK(!accepted);
    }
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(m_wallet, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_CHECKPOINT_INCOMPLETE_OPERATION");
    wallet::CWallet restarted{m_node.chain.get(), "pending-restart", std::make_unique<MockableDatabase>(database.m_records)};
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(restarted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_CHECKPOINT_INCOMPLETE_OPERATION");
    BOOST_CHECK(!WalletBatch{restarted.GetDatabase()}.WritePaymasterSettings(environment.settings));
}

BOOST_AUTO_TEST_CASE(checkpoint_native_journal_repairs_only_adjacent_exact_commit_gaps)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    PaymasterMaintenanceExecution execution;
    execution.genesis_hash = Params().GenesisBlock().GetHash();
    execution.provider_id = environment.identity.provider_id;
    execution.operation_id = GetRandHash();
    execution.created_at = 1000;
    CMutableTransaction transaction;
    transaction.vin.emplace_back(COutPoint{GetRandHash(), 0}, CScript{} << OP_TRUE);
    transaction.vout.emplace_back(1, environment.identity.identity_script);
    PaymasterMaintenanceExecutionLeg leg;
    leg.transaction = paymaster_store::internal::CanonicalBytes(transaction);
    leg.fee = DGBSatoshis{1};
    execution.legs.push_back(leg);
    std::string error;
    LOCK(m_wallet.cs_wallet);
    {
        PaymasterCheckpointOperation operation{m_wallet};
        BOOST_REQUIRE_MESSAGE(operation.Begin(execution, error), error);
        BOOST_REQUIRE(operation.Active());
    }
    const auto original = database.m_records;
    BOOST_CHECK(!CheckPaymasterCheckpoint(database, error));
    {
        PaymasterCheckpointOperation operation{m_wallet};
        BOOST_REQUIRE_MESSAGE(operation.Resume(execution, error), error);
        database.FailCommit();
        BOOST_CHECK(!WalletBatch{database}.WritePaymasterSettings(environment.settings));
        database.ClearFailureInjection();
    }
    // The external pending checkpoint is one commit ahead. Its immutable
    // token permits only this journal; a different operation cannot repair it.
    auto changed = execution;
    changed.operation_id = GetRandHash();
    {
        PaymasterCheckpointOperation operation{m_wallet};
        const auto before = database.m_records;
        BOOST_CHECK(!operation.Resume(changed, error));
        BOOST_CHECK(database.m_records == before);
        BOOST_REQUIRE_MESSAGE(operation.Resume(execution, error), error);
        database.FailCommit();
        BOOST_CHECK(!operation.Complete(error));
        database.ClearFailureInjection();
    }
    // Completion advanced the file but failed to erase the journal/marker in
    // the wallet. Repair that adjacent completion and finish atomically.
    {
        PaymasterCheckpointOperation operation{m_wallet};
        BOOST_REQUIRE_MESSAGE(operation.Resume(execution, error), error);
        BOOST_REQUIRE_MESSAGE(operation.Complete(error), error);
    }
    BOOST_REQUIRE_MESSAGE(CheckPaymasterCheckpoint(database, error), error);
    PaymasterMaintenanceExecution saved;
    BOOST_CHECK(WalletBatch{database}.ReadPaymasterMaintenanceExecutionWithStatus(saved) == DatabaseReadStatus::NOT_FOUND);
    wallet::CWallet clone{m_node.chain.get(), "old-native-journal", std::make_unique<MockableDatabase>(original)};
    LOCK(clone.cs_wallet);
    const auto clone_before = GetMockableDatabase(clone).m_records;
    PaymasterCheckpointOperation stale{clone};
    BOOST_CHECK(!stale.Resume(execution, error));
    BOOST_CHECK(GetMockableDatabase(clone).m_records == clone_before);
}

BOOST_AUTO_TEST_CASE(checkpoint_recovery_retirement_fences_other_copies)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    WalletBatch batch{database};
    BOOST_REQUIRE(batch.WritePaymasterIdentity(environment.identity));
    const auto original_records = database.m_records;
    wallet::CWallet original{m_node.chain.get(), "original-before-recovery", std::make_unique<MockableDatabase>(original_records)};
    std::string error;
    uint256 revision;
    BOOST_CHECK(!CheckPaymasterCapitalRecoveryCheckpoint(database, revision, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPITAL_RECOVERY_REQUIRES_RESTORED_WALLET");
    bool guarded{false};
    BOOST_REQUIRE(MarkPaymasterProviderRestored(database, guarded, error));
    BOOST_REQUIRE(guarded);
    const auto guarded_records = database.m_records;
    BOOST_REQUIRE(CheckPaymasterCapitalRecoveryCheckpoint(database, revision, error));
    BOOST_CHECK(database.m_records == guarded_records);
    BOOST_REQUIRE(batch.TxnBegin());
    BOOST_CHECK(!batch.RetirePaymasterCheckpoint(uint256::ONE));
    BOOST_REQUIRE(batch.TxnAbort());
    BOOST_CHECK(database.m_records == guarded_records);
    BOOST_REQUIRE(CheckPaymasterCheckpoint(original.GetDatabase(), error));
    BOOST_REQUIRE(batch.TxnBegin());
    BOOST_REQUIRE(batch.RetirePaymasterCheckpoint(revision));
    BOOST_REQUIRE(batch.WritePaymasterSettings(environment.settings));
    BOOST_REQUIRE(batch.TxnCommit());
    PaymasterCheckpoint retired;
    BOOST_REQUIRE(database.MakeBatch()->Read(std::string{"pmcheckpoint"}, retired));
    BOOST_CHECK_EQUAL(retired.pending, PaymasterCheckpoint::RETIRED);
    BOOST_CHECK(!CheckPaymasterCheckpoint(database, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_CHECKPOINT_RETIRED");
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(m_wallet, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    BOOST_CHECK(!CheckPaymasterCheckpoint(original.GetDatabase(), error));
    BOOST_CHECK(!WalletBatch{original.GetDatabase()}.WritePaymasterSettings(environment.settings));
    // Restarted copies cannot inherit any release permission or resume signing.
    wallet::CWallet restarted{m_node.chain.get(), "recovered-restart", std::make_unique<MockableDatabase>(database.m_records)};
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(restarted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    BOOST_REQUIRE(CheckPaymasterCapitalRecoveryCheckpoint(database, revision, error));
}

BOOST_AUTO_TEST_CASE(checkpoint_backup_reminder_does_not_obsolete_current_backup)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    const auto backup = database.m_records;
    PaymasterCheckpoint checkpoint;
    BOOST_REQUIRE(database.MakeBatch()->Read(std::string{"pmcheckpoint"}, checkpoint));
    std::string error;
    BOOST_REQUIRE(MarkPaymasterProviderBackupCompleted(m_wallet, environment.identity.created_at + 1, error));
    BOOST_REQUIRE(AcknowledgePaymasterProviderExternalBackup(m_wallet, environment.identity.created_at + 2, error));
    PaymasterCheckpoint after;
    BOOST_REQUIRE(database.MakeBatch()->Read(std::string{"pmcheckpoint"}, after));
    BOOST_CHECK(after == checkpoint);
    wallet::CWallet restored{m_node.chain.get(), "current-backup", std::make_unique<MockableDatabase>(backup)};
    bool guarded{false};
    BOOST_REQUIRE(MarkPaymasterProviderRestored(restored.GetDatabase(), guarded, error));
    BOOST_REQUIRE(guarded);
    uint256 revision;
    BOOST_REQUIRE(CheckPaymasterCapitalRecoveryCheckpoint(restored.GetDatabase(), revision, error));
    // Real settings/authority changes must still invalidate that same backup.
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterSettings(environment.settings));
    BOOST_CHECK(!CheckPaymasterCapitalRecoveryCheckpoint(restored.GetDatabase(), revision, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED");
}

BOOST_AUTO_TEST_CASE(checkpoint_version_one_upgrades_without_reenrollment)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    PaymasterCheckpoint legacy;
    BOOST_REQUIRE(database.MakeBatch()->Read(std::string{"pmcheckpoint"}, legacy));
    legacy.version = 1;
    CDataStream bytes{SER_DISK, CLIENT_VERSION};
    bytes << uint64_t{0x3150435042474444} << legacy
          << (HashWriter{} << std::string{"DigiByte/PaymasterCheckpoint/v1"} << legacy).GetHash();
    const auto path = PaymasterCheckpointPath(legacy.genesis_hash, legacy.provider_id);
    FILE* file = fsbridge::fopen(path, "wb");
    BOOST_REQUIRE(file);
    BOOST_REQUIRE_EQUAL(std::fwrite(bytes.data(), 1, bytes.size(), file), bytes.size());
    BOOST_REQUIRE_EQUAL(std::fclose(file), 0);
    BOOST_REQUIRE(database.MakeBatch()->Write(std::string{"pmcheckpoint"}, legacy));
    std::string error;
    BOOST_REQUIRE(CheckPaymasterCheckpoint(database, error));
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterSettings(environment.settings));
    PaymasterCheckpoint upgraded;
    BOOST_REQUIRE(database.MakeBatch()->Read(std::string{"pmcheckpoint"}, upgraded));
    BOOST_CHECK_EQUAL(upgraded.version, PaymasterCheckpoint::CURRENT_VERSION);
    BOOST_CHECK_EQUAL(upgraded.generation, legacy.generation + 1);
    BOOST_CHECK(upgraded.token != legacy.token);
    BOOST_REQUIRE(CheckPaymasterCheckpoint(database, error));
}

BOOST_AUTO_TEST_CASE(provider_restore_guard_is_durable_and_blocks_new_signatures)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    std::string error;
    bool guarded{true};
    // A client-only backup must retain its normal behavior.
    BOOST_REQUIRE(MarkPaymasterProviderRestored(database, guarded, error));
    BOOST_CHECK(!guarded);
    BOOST_REQUIRE(CheckPaymasterProviderRestoreGuard(m_wallet, error));
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    // An unmarked identity alone cannot distinguish a current wallet from a
    // manually copied old image. The explicit restore must persist the guard.
    BOOST_REQUIRE(CheckPaymasterProviderRestoreGuard(m_wallet, error));
    BOOST_REQUIRE(MarkPaymasterProviderRestored(database, guarded, error));
    BOOST_CHECK(guarded);
    BOOST_REQUIRE(MarkPaymasterProviderRestored(database, guarded, error));
    BOOST_CHECK(guarded);
    BOOST_CHECK(!WalletBatch{database}.WritePaymasterRestoreGuard(environment.identity));

    wallet::CWallet restarted{m_node.chain.get(), "restored-provider", std::make_unique<MockableDatabase>(database.m_records)};
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(restarted, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    // Configuration changes and backup acknowledgements are not history proof.
    BOOST_REQUIRE(WalletBatch{restarted.GetDatabase()}.WritePaymasterSettings(environment.settings));
    BOOST_REQUIRE(AcknowledgePaymasterProviderExternalBackup(restarted, 200001, error));
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(restarted, error));
    PaymasterStore store{restarted};
    PaymasterCapacityRequest request;
    PaymasterCapacityProof proof;
    BOOST_CHECK(!store.ValidateProviderPreSignatureAuthorization({}, Params().GenesisBlock().GetHash(), 200001, request, proof, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    BOOST_CHECK(!store.ValidateProviderAlternativeRecoveryPreSignatureAuthorization({}, Params().GenesisBlock().GetHash(), 200001, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    PartiallySignedTransaction psbt;
    BOOST_CHECK(!SignCollaborativePSBTForParty(restarted, psbt, {}, SigningParty::PROVIDER, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    WITH_LOCK(restarted.cs_wallet, restarted.SetWalletFlag(WALLET_FLAG_DESCRIPTORS));
    CKey key;
    ProviderIdentityRecord identity;
    BOOST_CHECK(!GetPaymasterIdentityKey(restarted, key, identity, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    ProviderLiquidityPolicy liquidity;
    BOOST_CHECK(!paymaster_rpc::internal::RunAutomaticDGBReplenishment(restarted, liquidity, 1, 1, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    BOOST_CHECK(!paymaster_rpc::internal::RunAutomaticCarrierReplenishment(restarted, liquidity, 1, 1, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
}

BOOST_AUTO_TEST_CASE(automatic_pool_funding_requires_broadcast_before_side_effects)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    const COutPoint manually_locked{uint256::ONE, 7};
    m_wallet.LockCoin(manually_locked);
    m_wallet.SetBroadcastTransactions(false);
    const auto before = database.m_records;
    const auto transactions_before = m_wallet.mapWallet.size();
    ProviderLiquidityPolicy policy;
    policy.automatic_replenishment = true;
    policy.paid_maintenance_approved = true;
    policy.updated_at = 200000;
    for (const bool carrier : {false, true}) {
        std::string error;
        const auto run = carrier ? paymaster_app::RunAutomaticCarrierReplenishment
                                 : paymaster_app::RunAutomaticDGBReplenishment;
        BOOST_CHECK(!run(m_wallet, policy, 1, 1, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_WALLET_BROADCAST_DISABLED");
        BOOST_CHECK(database.m_records == before);
        BOOST_CHECK_EQUAL(m_wallet.mapWallet.size(), transactions_before);
        BOOST_CHECK(m_wallet.IsOrdinaryLockedCoin(manually_locked));
        // An already satisfied target remains a no-op, including offline.
        BOOST_CHECK(run(m_wallet, policy, 0, 0, error));
        BOOST_CHECK(error.empty());
        BOOST_CHECK(database.m_records == before);
    }
    std::string error;
    BOOST_CHECK(!paymaster_app::CheckPaymasterPoolFundingBroadcast(m_wallet, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_WALLET_BROADCAST_DISABLED");
    m_wallet.SetBroadcastTransactions(true);
    BOOST_CHECK(paymaster_app::CheckPaymasterPoolFundingBroadcast(m_wallet, error));
    BOOST_CHECK(error.empty());
    BOOST_CHECK(database.m_records == before);
}

BOOST_AUTO_TEST_CASE(saved_maintenance_is_not_a_broadcast_observation)
{
    m_wallet.SetLastBlockProcessed(1, Params().GenesisBlock().GetHash());
    CMutableTransaction saved;
    saved.vin.emplace_back(COutPoint{uint256::ONE, 0});
    saved.vout.emplace_back(COIN, CScript{} << OP_TRUE);
    const auto transaction = MakeTransactionRef(saved);
    BOOST_REQUIRE(m_wallet.AddToWallet(transaction, TxStateInactive{}));
    ProviderMaintenanceRecord record;
    record.state = ProviderMaintenanceState::BROADCAST;
    record.transaction_id = transaction->GetHash();
    ProviderMaintenanceLedger ledger;
    ledger.records.push_back(record);
    auto& database = GetMockableDatabase(m_wallet);
    const auto before = database.m_records;
    const uint256 witness_hash = transaction->GetWitnessHash();
    m_wallet.SetBroadcastTransactions(false);
    BOOST_CHECK(paymaster_app::ProviderMaintenanceBroadcastPending(m_wallet, ledger));
    m_wallet.SetBroadcastTransactions(true);
    BOOST_CHECK(paymaster_app::ProviderMaintenanceBroadcastPending(m_wallet, ledger));
    auto& wallet_transaction = m_wallet.mapWallet.at(transaction->GetHash());
    wallet_transaction.m_state = TxStateInMempool{};
    BOOST_CHECK(!paymaster_app::ProviderMaintenanceBroadcastPending(m_wallet, ledger));
    wallet_transaction.m_state = TxStateConfirmed{Params().GenesisBlock().GetHash(), 0, 0};
    BOOST_CHECK(!paymaster_app::ProviderMaintenanceBroadcastPending(m_wallet, ledger));
    BOOST_CHECK(wallet_transaction.tx->GetWitnessHash() == witness_hash);
    BOOST_CHECK(database.m_records == before);
}

BOOST_AUTO_TEST_CASE(provider_restore_marker_failure_prevents_restore_completion)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    auto& database = GetMockableDatabase(m_wallet);
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity));
    const MockableData original = database.m_records;
    std::string error;
    bool guarded{false};
    for (bool fail_commit : {false, true}) {
        if (fail_commit) database.FailCommit();
        else database.FailWriteAt(0);
        BOOST_CHECK(!MarkPaymasterProviderRestored(database, guarded, error));
        BOOST_CHECK(!guarded);
        BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_DATABASE_WRITE");
        BOOST_CHECK(database.m_records == original);
        database.ClearFailureInjection();
    }
    BOOST_REQUIRE(MarkPaymasterProviderRestored(database, guarded, error));
    BOOST_CHECK(guarded);
}

BOOST_AUTO_TEST_CASE(provider_restore_marker_corruption_and_future_versions_fail_closed)
{
    auto& database = m_wallet.GetDatabase();
    std::string error;
    bool guarded{false};
    // A malformed identity is not a client-only wallet.
    BOOST_REQUIRE(database.MakeBatch()->Write(DBKeys::PAYMASTER_IDENTITY, uint16_t{65535}));
    BOOST_CHECK(!MarkPaymasterProviderRestored(database, guarded, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_IDENTITY_INVALID");
    const auto environment = MakeProviderEnvironment(200000, 2000);
    BOOST_REQUIRE(database.MakeBatch()->Erase(DBKeys::PAYMASTER_IDENTITY));
    BOOST_REQUIRE(WalletBatch{database}.WritePaymasterIdentity(environment.identity, /*overwrite=*/true));
    for (uint16_t version : {uint16_t{1}, uint16_t{65535}}) {
        BOOST_REQUIRE(database.MakeBatch()->Write(DBKeys::PAYMASTER_RESTORE_GUARD, version));
        BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(m_wallet, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
        const MockableData before_restore = GetMockableDatabase(m_wallet).m_records;
        BOOST_CHECK(!MarkPaymasterProviderRestored(database, guarded, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
        BOOST_CHECK(GetMockableDatabase(m_wallet).m_records == before_restore);
    }
}

#ifdef USE_SQLITE
BOOST_AUTO_TEST_CASE(sqlite_full_quote_preserves_budget_pool_and_retry)
{
    constexpr int64_t now{1000};
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655441095";
    auto environment = MakeProviderEnvironment(now, /*maximum_reserved=*/2000);
    ProviderQuoteFixture fixture;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(environment, request_id,
        uint256S("1095"), now, fixture, error), error);
    const auto path = m_path_root / "paymaster-sqlite-full";
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str database_error;
    uint256 before;
    {
        auto database = MakeSQLiteDatabase(path, options, status, database_error);
        BOOST_REQUIRE_MESSAGE(database, database_error.original);
        auto* const sqlite = database.get();
        wallet::CWallet wallet{m_node.chain.get(), "sqlite-full-provider", std::move(database)};
        BOOST_REQUIRE(PersistProviderQuoteEnvironment(wallet, environment, {fixture.pool_entry}));
        before = SQLiteRecordDigest(wallet.GetDatabase());
        const int pages = SQLiteInteger(sqlite->m_db, "PRAGMA page_count");
        BOOST_REQUIRE_EQUAL(SQLiteInteger(sqlite->m_db,
            "PRAGMA max_page_count=" + std::to_string(pages)), pages);
        // Real SQLITE_FULL from a bounded engine, without filling a disk.
        BOOST_REQUIRE_EQUAL(sqlite3_exec(sqlite->m_db,
            "INSERT INTO main(key,value) VALUES(X'00',zeroblob(1048576))",
            nullptr, nullptr, nullptr), SQLITE_FULL);
        BOOST_CHECK(SQLiteRecordDigest(wallet.GetDatabase()) == before);
        PaymasterStore store{wallet};
        BOOST_CHECK(!store.CommitProviderQuote(fixture.attempt,
            fixture.request.intent.genesis_hash, now, error));
        BOOST_CHECK(error == "PAYMASTER_DATABASE_WRITE" || error == "PAYMASTER_DATABASE_COMMIT");
        BOOST_CHECK_EQUAL(sqlite3_get_autocommit(sqlite->m_db), 1);
        BOOST_CHECK(SQLiteRecordDigest(wallet.GetDatabase()) == before);
    }
    options.require_existing = true;
    auto reopened = MakeSQLiteDatabase(path, options, status, database_error);
    BOOST_REQUIRE_MESSAGE(reopened, database_error.original);
    BOOST_CHECK(SQLiteRecordDigest(*reopened) == before);
    // The page limit is connection-local. Reopening restores capacity; the
    // exact request must succeed once, without duplicated budget or indices.
    wallet::CWallet restarted{m_node.chain.get(), "sqlite-full-restart", std::move(reopened)};
    PaymasterStore store{restarted};
    BOOST_REQUIRE_MESSAGE(store.CommitProviderQuote(fixture.attempt,
        fixture.request.intent.genesis_hash, now, error), error);
    ProviderBudgetLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(ReadProviderSecurityState(restarted, ledger, pool));
    BOOST_REQUIRE_EQUAL(ledger.reservations.size(), 1U);
    BOOST_REQUIRE_EQUAL(pool.size(), 1U);
    BOOST_CHECK(ledger.reservations.front().commit_key == fixture.attempt.commit_key);
    BOOST_CHECK(pool.front().reservation_id == fixture.attempt.commit_key);
    const auto committed = SQLiteRecordDigest(restarted.GetDatabase());
    BOOST_REQUIRE_MESSAGE(store.CommitProviderQuote(fixture.attempt,
        fixture.request.intent.genesis_hash, now + 1, error), error);
    BOOST_CHECK(SQLiteRecordDigest(restarted.GetDatabase()) == committed);
}

BOOST_AUTO_TEST_CASE(checkpoint_sqlite_commit_failure_retains_independent_high_water_mark)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str database_error;
    auto database = MakeSQLiteDatabase(m_path_root / "checkpoint-commit", options, status, database_error);
    BOOST_REQUIRE_MESSAGE(database, database_error.original);
    BOOST_REQUIRE(WalletBatch{*database}.WritePaymasterIdentity(environment.identity));
    const auto before = SQLiteRecordDigest(*database);
    {
        SQLiteCommitFailure failure{database->m_db};
        BOOST_CHECK(!WalletBatch{*database}.WritePaymasterSettings(environment.settings));
        BOOST_CHECK_EQUAL(failure.denied_commits, 1U);
        BOOST_CHECK_EQUAL(sqlite3_get_autocommit(database->m_db), 1);
    }
    BOOST_CHECK(SQLiteRecordDigest(*database) == before);
    std::string error;
    BOOST_CHECK(!CheckPaymasterCheckpoint(*database, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_CHECKPOINT_REVIEW_REQUIRED");
    BOOST_CHECK(!WalletBatch{*database}.WritePaymasterSettings(environment.settings));
}

BOOST_AUTO_TEST_CASE(sqlite_failed_restore_commit_cannot_leave_partial_guard)
{
    const auto environment = MakeProviderEnvironment(200000, 2000);
    const auto path = m_path_root / "paymaster-sqlite-restore-commit";
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str database_error;
    auto database = MakeSQLiteDatabase(path, options, status, database_error);
    BOOST_REQUIRE_MESSAGE(database, database_error.original);
    BOOST_REQUIRE(WalletBatch{*database}.WritePaymasterIdentity(environment.identity));
    const auto before = SQLiteRecordDigest(*database);
    bool guarded{false};
    std::string error;
    {
        SQLiteCommitFailure failure{database->m_db};
        BOOST_CHECK(!MarkPaymasterProviderRestored(*database, guarded, error));
        BOOST_CHECK_EQUAL(failure.denied_commits, 1U);
        BOOST_CHECK(!guarded);
        BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_DATABASE_WRITE");
        BOOST_CHECK_EQUAL(sqlite3_get_autocommit(database->m_db), 1);
        BOOST_CHECK(SQLiteRecordDigest(*database) == before);
    }
    database.reset();
    options.require_existing = true;
    database = MakeSQLiteDatabase(path, options, status, database_error);
    BOOST_REQUIRE_MESSAGE(database, database_error.original);
    BOOST_CHECK(SQLiteRecordDigest(*database) == before);
    BOOST_REQUIRE_MESSAGE(MarkPaymasterProviderRestored(*database, guarded, error), error);
    BOOST_CHECK(guarded);
    const auto protected_image = SQLiteRecordDigest(*database);
    database.reset();
    auto reopened = MakeSQLiteDatabase(path, options, status, database_error);
    BOOST_REQUIRE_MESSAGE(reopened, database_error.original);
    BOOST_CHECK(SQLiteRecordDigest(*reopened) == protected_image);
    wallet::CWallet restored{m_node.chain.get(), "sqlite-guard-restart", std::move(reopened)};
    BOOST_CHECK(!CheckPaymasterProviderRestoreGuard(restored, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
}
#endif

BOOST_AUTO_TEST_CASE(provider_policy_update_preserves_safety_policy_liveness)
{
    constexpr int64_t now{900};
    ProviderSecurityEnvironment environment =
        MakeProviderEnvironment(now, /*maximum_reserved=*/2000);
    ProviderQuoteFixture stale_quote;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(
                              environment,
                              "550e8400-e29b-41d4-a716-446655441000",
                              uint256S("0900"), now, stale_quote, error),
                          error);
    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    }
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(
        m_wallet, environment, {stale_quote.pool_entry}));

    auto& database = GetMockableDatabase(m_wallet);
    ProviderPolicy incompatible{environment.policy};
    incompatible.maximum_network_fee = DGBSatoshis{499};
    BOOST_CHECK(!ValidateProviderSafetyPolicy(
        environment.safety, incompatible, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_SAFETY_LIMITS");
    const MockableData before_incompatible = database.m_records;
    BOOST_CHECK(!SetPaymasterProviderPolicy(
        m_wallet, incompatible, now + 1, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_SAFETY_POLICY_CONFLICT");
    BOOST_CHECK(database.m_records == before_incompatible);

    ProviderPolicy persisted_policy;
    ProviderSafetyPolicy persisted_safety;
    ProviderSettings persisted_settings;
    BOOST_REQUIRE(GetPaymasterProviderPolicy(m_wallet, persisted_policy));
    BOOST_REQUIRE(GetPaymasterProviderSafetyPolicy(m_wallet,
                                                   persisted_safety));
    BOOST_REQUIRE(GetPaymasterProviderSettings(m_wallet, persisted_settings));
    BOOST_CHECK_EQUAL(GetProviderPolicyHash(persisted_policy),
                      GetProviderPolicyHash(environment.policy));
    BOOST_CHECK_EQUAL(GetProviderSafetyPolicyHash(persisted_safety),
                      GetProviderSafetyPolicyHash(environment.safety));
    BOOST_CHECK_EQUAL(persisted_settings.policy_hash,
                      GetProviderPolicyHash(environment.policy));
    BOOST_CHECK_EQUAL(persisted_settings.enabled,
                      environment.settings.enabled);
    BOOST_CHECK_EQUAL(persisted_settings.updated_at,
                      environment.settings.updated_at);

    ProviderPolicy compatible{environment.policy};
    compatible.min_payment.value++;
    BOOST_REQUIRE_MESSAGE(ValidateProviderPolicy(compatible, error), error);
    BOOST_REQUIRE_MESSAGE(ValidateProviderSafetyPolicy(
                              environment.safety, compatible, error),
                          error);
    const MockableData before_compatible = database.m_records;
    database.FailWriteAt(1);
    BOOST_CHECK(!SetPaymasterProviderPolicy(
        m_wallet, compatible, now + 2, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_compatible);
    BOOST_REQUIRE(GetPaymasterProviderPolicy(m_wallet, persisted_policy));
    BOOST_REQUIRE(GetPaymasterProviderSettings(m_wallet, persisted_settings));
    BOOST_CHECK_EQUAL(GetProviderPolicyHash(persisted_policy),
                      GetProviderPolicyHash(environment.policy));
    BOOST_CHECK_EQUAL(persisted_settings.policy_hash,
                      GetProviderPolicyHash(environment.policy));
    BOOST_CHECK_EQUAL(persisted_settings.enabled,
                      environment.settings.enabled);
    BOOST_CHECK_EQUAL(persisted_settings.updated_at,
                      environment.settings.updated_at);

    database.FailCommit();
    BOOST_CHECK(!SetPaymasterProviderPolicy(
        m_wallet, compatible, now + 2, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_compatible);
    BOOST_REQUIRE(GetPaymasterProviderPolicy(m_wallet, persisted_policy));
    BOOST_REQUIRE(GetPaymasterProviderSettings(m_wallet, persisted_settings));
    BOOST_CHECK_EQUAL(GetProviderPolicyHash(persisted_policy),
                      GetProviderPolicyHash(environment.policy));
    BOOST_CHECK_EQUAL(persisted_settings.policy_hash,
                      GetProviderPolicyHash(environment.policy));
    BOOST_CHECK_EQUAL(persisted_settings.enabled,
                      environment.settings.enabled);
    BOOST_CHECK_EQUAL(persisted_settings.updated_at,
                      environment.settings.updated_at);

    AlignSecurityFixtureCheckpoint(database);
    BOOST_REQUIRE_MESSAGE(SetPaymasterProviderPolicy(
                              m_wallet, compatible, now + 2, error),
                          error);
    BOOST_REQUIRE(GetPaymasterProviderPolicy(m_wallet, persisted_policy));
    BOOST_REQUIRE(GetPaymasterProviderSafetyPolicy(m_wallet,
                                                   persisted_safety));
    BOOST_REQUIRE(GetPaymasterProviderSettings(m_wallet, persisted_settings));
    BOOST_CHECK_EQUAL(GetProviderPolicyHash(persisted_policy),
                      GetProviderPolicyHash(compatible));
    BOOST_CHECK_EQUAL(GetProviderSafetyPolicyHash(persisted_safety),
                      GetProviderSafetyPolicyHash(environment.safety));
    BOOST_CHECK_EQUAL(persisted_settings.policy_hash,
                      GetProviderPolicyHash(compatible));
    BOOST_CHECK_EQUAL(persisted_settings.enabled,
                      environment.settings.enabled);
    BOOST_CHECK_EQUAL(persisted_settings.updated_at, now + 2);

    PaymasterStore store{m_wallet};
    const MockableData before_stale_quote = database.m_records;
    BOOST_CHECK(!store.CommitProviderQuote(
        stale_quote.attempt, stale_quote.request.intent.genesis_hash,
        now + 3, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_QUOTE_BINDING_MISMATCH");
    BOOST_CHECK(database.m_records == before_stale_quote);
    ProviderAttempt absent;
    BOOST_CHECK(!store.GetAttempt(stale_quote.attempt.attempt_id, absent));
}

BOOST_AUTO_TEST_CASE(provider_quote_commit_is_atomic_and_restart_durable)
{
    constexpr int64_t now{1000};
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655441001";
    ProviderSecurityEnvironment environment =
        MakeProviderEnvironment(now, /*maximum_reserved=*/2000);
    ProviderQuoteFixture fixture;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(
                              environment, request_id, uint256S("1001"), now,
                              fixture, error),
                          error);
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(
        m_wallet, environment, {fixture.pool_entry}));

    BOOST_REQUIRE_EQUAL(environment.ledger.capacity_admissions.size(), 1U);
    const ProviderCapacityAdmission& reserved_admission =
        environment.ledger.capacity_admissions.front();
    BOOST_CHECK(reserved_admission.state ==
                CapacityAdmissionState::RESERVED);
    BOOST_CHECK(reserved_admission.quote_request_hash.IsNull());
    BOOST_CHECK(reserved_admission.commit_key.IsNull());
    BOOST_CHECK(environment.ledger.reservations.empty());

    PaymasterStore store{m_wallet};
    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_quote = database.m_records;
    const auto reject_noncanonical_without_write =
        [&](ProviderAttempt candidate,
            std::initializer_list<std::string_view> expected_errors) {
            BOOST_CHECK(!store.CommitProviderQuote(
                std::move(candidate), fixture.request.intent.genesis_hash,
                now, error));
            BOOST_CHECK(std::find(expected_errors.begin(), expected_errors.end(),
                                  error) != expected_errors.end());
            BOOST_CHECK(database.m_records == before_quote);
        };
    ProviderAttempt trailing_request{fixture.attempt};
    trailing_request.quote_request.push_back(0);
    reject_noncanonical_without_write(
        std::move(trailing_request), {"PAYMASTER_PROVIDER_QUOTE_ENCODING"});
    ProviderAttempt trailing_response{fixture.attempt};
    trailing_response.signed_quote.push_back(0);
    reject_noncanonical_without_write(
        std::move(trailing_response), {"PAYMASTER_PROVIDER_QUOTE_ENCODING"});
    ProviderAttempt trailing_transaction{fixture.attempt};
    trailing_transaction.unsigned_transaction.push_back(0);
    reject_noncanonical_without_write(
        std::move(trailing_transaction),
        {"PAYMASTER_TEMPLATE_TRANSACTION_ENCODING"});
    ProviderAttempt trailing_psbt{fixture.attempt};
    trailing_psbt.unsigned_psbt.push_back(0);
    reject_noncanonical_without_write(
        std::move(trailing_psbt),
        {"PAYMASTER_TEMPLATE_PSBT_ENCODING",
         "PAYMASTER_TEMPLATE_PSBT_NONCANONICAL"});
    for (size_t failing_write = 0; failing_write < 7; ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.CommitProviderQuote(
            fixture.attempt, fixture.request.intent.genesis_hash, now,
            error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before_quote);
        ProviderAttempt absent;
        BOOST_CHECK(!store.GetAttempt(fixture.attempt.attempt_id, absent));
        PaymentSession absent_session;
        BOOST_CHECK(!store.GetSessionByRequestId(request_id, absent_session));
    }
    database.FailCommit();
    BOOST_CHECK(!store.CommitProviderQuote(
        fixture.attempt, fixture.request.intent.genesis_hash, now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_quote);
    AlignSecurityFixtureCheckpoint(database);

    BOOST_REQUIRE_MESSAGE(
        store.CommitProviderQuote(
            fixture.attempt, fixture.request.intent.genesis_hash, now,
            error),
        error);
    PaymentSession session;
    ProviderAttempt attempt;
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_REQUIRE(store.GetAttempt(fixture.attempt.attempt_id, attempt));
    BOOST_CHECK(session.provider_side);
    BOOST_CHECK(session.state == SessionState::INPUTS_RESERVED);
    BOOST_CHECK(attempt.state == AttemptState::QUOTED);

    ProviderBudgetLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(ReadProviderSecurityState(m_wallet, ledger, pool));
    BOOST_REQUIRE_EQUAL(pool.size(), 1U);
    BOOST_CHECK(pool.front().state == PoolEntryState::RESERVED);
    BOOST_CHECK_EQUAL(pool.front().reservation_id, fixture.attempt.commit_key);
    const ProviderBudgetReservation* reservation =
        FindBudgetReservation(ledger, fixture.attempt.commit_key);
    BOOST_REQUIRE(reservation != nullptr);
    BOOST_CHECK(reservation->state == BudgetReservationState::RESERVED);
    BOOST_CHECK(fixture.attempt.provider_manifest.budget_reservation_id ==
                fixture.attempt.commit_key);
    BOOST_CHECK(fixture.attempt.provider_manifest.maximum_network_fee ==
                fixture.response.quote.network_fee);
    BOOST_REQUIRE_MESSAGE(store.ValidateProviderBudgetAuthorization(
                              attempt, BudgetReservationState::RESERVED,
                              /*allow_historical_policy=*/false, error),
                          error);

    ProviderAuthorizationManifest changed_budget_manifest{
        attempt.provider_manifest};
    changed_budget_manifest.maximum_network_fee.value++;
    changed_budget_manifest.manifest_id =
        GetProviderAuthorizationManifestId(changed_budget_manifest);
    ProviderAttempt changed_budget_attempt{attempt};
    changed_budget_attempt.provider_manifest = changed_budget_manifest;
    BOOST_CHECK(!ValidateProviderBudgetReservationBinding(
        changed_budget_manifest, changed_budget_attempt, *reservation,
        environment.safety, BudgetReservationState::RESERVED,
        /*allow_historical_policy=*/false, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_BUDGET_BINDING_MISMATCH");

    ProviderBudgetReservation changed_reservation{*reservation};
    changed_reservation.network_fee.value++;
    BOOST_CHECK(!ValidateProviderBudgetReservationBinding(
        attempt.provider_manifest, attempt, changed_reservation,
        environment.safety, BudgetReservationState::RESERVED,
        /*allow_historical_policy=*/false, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_BUDGET_BINDING_MISMATCH");
    const ProviderCapacityAdmission* admission =
        FindCapacityAdmission(ledger, fixture.attempt.commit_key);
    BOOST_REQUIRE(admission != nullptr);
    BOOST_CHECK(admission->state == CapacityAdmissionState::PROMOTED);

    bool has_work{false};
    BOOST_REQUIRE(store.HasProviderDrainWork(
        environment.identity.provider_id, has_work, error));
    BOOST_CHECK(has_work);

    auto restarted_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    wallet::CWallet restarted_wallet{m_node.chain.get(),
                                     "quote-security-restart",
                                     std::move(restarted_database)};
    PaymasterStore restarted_store{restarted_wallet};
    BOOST_REQUIRE(restarted_store.GetSessionByRequestId(request_id, session));
    BOOST_REQUIRE(restarted_store.GetAttempt(fixture.attempt.attempt_id,
                                             attempt));
    BOOST_CHECK(session.provider_side);
    BOOST_CHECK(attempt.state == AttemptState::QUOTED);
    auto& restarted_mock = GetMockableDatabase(restarted_wallet);
    const MockableData restarted_before_retry = restarted_mock.m_records;
    restarted_mock.FailWriteAt(0);
    BOOST_CHECK(restarted_store.CommitProviderQuote(
        fixture.attempt, fixture.request.intent.genesis_hash, now + 1,
        error));
    restarted_mock.ClearFailureInjection();
    BOOST_CHECK(restarted_mock.m_records == restarted_before_retry);
    BOOST_REQUIRE(restarted_store.HasProviderDrainWork(
        environment.identity.provider_id, has_work, error));
    BOOST_CHECK(has_work);

    auto corrupt_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    wallet::CWallet corrupt_wallet{m_node.chain.get(),
                                   "quote-security-missing-budget",
                                   std::move(corrupt_database)};
    {
        LOCK(corrupt_wallet.cs_wallet);
        WalletBatch batch{corrupt_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(corrupt_wallet.GetDatabase());
        ProviderBudgetLedger corrupt_ledger;
        BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(corrupt_ledger));
        corrupt_ledger.reservations.clear();
        BOOST_REQUIRE(corrupt_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_PROVIDER_BUDGET, corrupt_ledger));
    }
    PaymasterStore corrupt_store{corrupt_wallet};
    BOOST_CHECK(!corrupt_store.CommitProviderQuote(
        fixture.attempt, fixture.request.intent.genesis_hash, now + 2,
        error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_MISSING");
    ProviderBudgetLedger still_missing;
    std::vector<ProviderPoolEntry> ignored_pool;
    BOOST_REQUIRE(ReadProviderSecurityState(
        corrupt_wallet, still_missing, ignored_pool));
    BOOST_CHECK(still_missing.reservations.empty());
}

BOOST_AUTO_TEST_CASE(provider_capacity_cache_compaction_outlives_pruned_normal_owner)
{
    constexpr int64_t now{1000};
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655441191";
    auto owned_database = std::make_unique<CapacityEraseFailureDatabase>();
    auto& database = *owned_database;
    wallet::CWallet wallet{m_node.chain.get(), "capacity-cache-pruned-owner", std::move(owned_database)};
    BOOST_REQUIRE(wallet.LoadWallet() == DBErrors::LOAD_OK);
    auto environment = MakeProviderEnvironment(now, /*maximum_reserved=*/2000);
    ProviderQuoteFixture fixture;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(environment, request_id,
        uint256S("1191"), now, fixture, error, nullptr, {}, DGBSatoshis{500},
        5500, Params().GenesisBlock().GetHash()), error);
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(wallet, environment, {fixture.pool_entry}));
    PaymasterStore store{wallet};
    BOOST_REQUIRE_MESSAGE(store.CommitProviderQuote(fixture.attempt,
        fixture.request.intent.genesis_hash, now, error), error);
    const auto& cache = environment.capacity_records.front();
    const int64_t boundary = fixture.capacity_proof.expires_at + DEFAULT_RETRY_SECONDS + MAX_QUOTE_TTL_SECONDS;
    size_t expired{0};
    const auto quoted = database.m_records;
    BOOST_REQUIRE_MESSAGE(store.ExpireProviderCapacityReservations(boundary, expired, error), error);
    BOOST_CHECK(database.m_records == quoted);

    // A historical normal owner can already have been pruned at depth 240,
    // before the retry horizon. Exercise the same storage prune/tombstone path;
    // this fixture does not claim transaction or chain validation.
    BOOST_REQUIRE(store.TransitionSession(request_id, SessionState::AUTHORIZED,
        PendingPhase::NONE, {}, now + 1, error));
    BOOST_REQUIRE(store.TransitionSession(request_id, SessionState::PENDING_PROVIDER,
        PendingPhase::USER_SIGNATURE_SENT, {}, now + 2, error));
    BOOST_REQUIRE(store.TransitionSession(request_id, SessionState::CONFIRMED,
        PendingPhase::NONE, uint256S("1192"), now + 3, error));
    BOOST_REQUIRE_MESSAGE(store.PruneFinalSession(request_id, error), error);
    PaymentSession session;
    ProviderAttempt attempt;
    IdempotencyTombstone tombstone;
    WalletBatch source{database};
    BOOST_CHECK(source.ReadPaymasterSessionWithStatus(request_id, session) == DatabaseReadStatus::NOT_FOUND);
    BOOST_CHECK(source.ReadPaymasterAttemptWithStatus(fixture.attempt.attempt_id, attempt) == DatabaseReadStatus::NOT_FOUND);
    BOOST_REQUIRE(source.ReadPaymasterTombstoneWithStatus(request_id, tombstone) == DatabaseReadStatus::FOUND);
    BOOST_CHECK(tombstone.session_id == fixture.attempt.session_id);
    const auto pruned = database.m_records;
    BOOST_REQUIRE_MESSAGE(store.ExpireProviderCapacityReservations(boundary, expired, error), error);
    BOOST_CHECK(database.m_records == pruned);

    // Cache authenticity and both index bindings remain mandatory, even when
    // no prunable owner survives. Unknown local formats are also protective.
    for (int scenario = 0; scenario < 4; ++scenario) {
        auto raw = database.MakeBatch();
        if (scenario == 0) {
            auto corrupt = fixture.capacity_proof;
            corrupt.identity_signature.back() ^= 1;
            BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESPONSE, cache.request_hash),
                SerializePaymasterSecurityObject(corrupt)));
        } else if (scenario == 1) {
            BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_CAPACITY_NONCE, cache.client_nonce), uint256::ONE));
        } else if (scenario == 2) {
            BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_CAPACITY_SESSION, cache.session_key), uint256::ONE));
        } else {
            BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RELEASE, cache.request_hash), uint16_t{65535}));
        }
        const auto corrupt = database.m_records;
        BOOST_CHECK(!store.ExpireProviderCapacityReservations(boundary + 1, expired, error));
        BOOST_CHECK(database.m_records == corrupt);
        BOOST_CHECK(GetPaymasterCapacityCleanupCursor(database).IsNull());
        database.m_records = pruned;
    }
    // Every cache/index/admission mutation and the atomic commit can fail
    // without modifying the historical fee reservation or input ownership.
    for (int failure = 0; failure < 7; ++failure) {
        database.erase_count = 0;
        if (failure == 0) database.m_txn_pass = false;
        else if (failure <= 3) database.fail_erase_at = failure - 1;
        else if (failure <= 5) database.FailWriteAt(failure - 4);
        else database.FailCommit();
        BOOST_CHECK(!store.ExpireProviderCapacityReservations(boundary + 1, expired, error));
        database.m_txn_pass = true;
        database.fail_erase_at.reset();
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == pruned);
        BOOST_CHECK(GetPaymasterCapacityCleanupCursor(database).IsNull());
        AlignSecurityFixtureCheckpoint(database);
    }
    ProviderBudgetLedger before_ledger;
    std::vector<ProviderPoolEntry> before_pool;
    BOOST_REQUIRE(ReadProviderSecurityState(wallet, before_ledger, before_pool));
    wallet::CWallet restarted{m_node.chain.get(), "capacity-cache-owner-pruned",
        DuplicateSecurityFixtureDatabase(database)};
    PaymasterStore restarted_store{restarted};
    BOOST_REQUIRE_MESSAGE(restarted_store.ExpireProviderCapacityReservations(boundary + 1, expired, error), error);
    BOOST_CHECK_EQUAL(expired, 0U); // Replay compaction never reports a financial release.
    WalletBatch batch{restarted.GetDatabase()};
    std::vector<unsigned char> bytes;
    uint256 indexed;
    BOOST_CHECK(batch.ReadPaymasterCapacityResponseWithStatus(cache.request_hash, bytes) == DatabaseReadStatus::NOT_FOUND);
    BOOST_CHECK(batch.ReadPaymasterCapacityNonceWithStatus(cache.client_nonce, indexed) == DatabaseReadStatus::NOT_FOUND);
    BOOST_CHECK(batch.ReadPaymasterCapacitySessionWithStatus(cache.session_key, indexed) == DatabaseReadStatus::NOT_FOUND);
    ProviderBudgetLedger after_ledger;
    std::vector<ProviderPoolEntry> after_pool;
    BOOST_REQUIRE(ReadProviderSecurityState(restarted, after_ledger, after_pool));
    before_ledger.capacity_admissions.clear();
    BOOST_CHECK(SerializePaymasterSecurityObject(before_ledger) == SerializePaymasterSecurityObject(after_ledger));
    BOOST_CHECK(SerializePaymasterSecurityObject(before_pool) == SerializePaymasterSecurityObject(after_pool));
    BOOST_REQUIRE(CheckPaymasterCheckpoint(restarted.GetDatabase(), error));
}

BOOST_AUTO_TEST_CASE(provider_capacity_replay_cleanup_is_bounded_and_restarts_after_failure)
{
    constexpr int64_t now{1000};
    auto environment = MakeProviderEnvironment(now, /*maximum_reserved=*/2000);
    ProviderQuoteFixture fixture;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(environment,
        "550e8400-e29b-41d4-a716-446655441192", uint256S("1193"), now,
        fixture, error, nullptr, {}, DGBSatoshis{500}, 5500, Params().GenesisBlock().GetHash()), error);
    environment.capacity_records.clear();
    environment.ledger.capacity_admissions.clear();
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(m_wallet, environment, {}));
    WalletBatch batch{m_wallet.GetDatabase()};
    BOOST_REQUIRE(batch.TxnBegin());
    const size_t total = MAX_PROVIDER_CAPACITY_CLEANUP_RESPONSES + 3;
    for (size_t i = 0; i < total; ++i) {
        auto proof = fixture.capacity_proof;
        auto request = fixture.capacity_request;
        request.request_id = proof.request_id = strprintf("550e8400-e29b-41d4-a716-%012u", i);
        request.session_id = proof.session_id = SecurityTestId(uint256S("1194"), i + 1);
        request.client_nonce = proof.client_nonce = SecurityTestId(uint256S("1195"), i + 1);
        BOOST_REQUIRE(environment.identity_key.SignSchnorr(GetCapacityProofSignatureHash(proof),
            proof.identity_signature, nullptr, uint256{}));
        const auto hash = Hash(SerializePaymasterSecurityObject(request));
        BOOST_REQUIRE(batch.WritePaymasterCapacityResponse(hash, SerializePaymasterSecurityObject(proof)));
        BOOST_REQUIRE(batch.WritePaymasterCapacityNonce(proof.client_nonce, hash));
        BOOST_REQUIRE(batch.WritePaymasterCapacitySession(SecurityCapacitySessionKey(proof), hash));
    }
    BOOST_REQUIRE(batch.TxnCommit());
    PaymasterStore store{m_wallet};
    const int64_t expiry = fixture.capacity_proof.expires_at + DEFAULT_RETRY_SECONDS + MAX_QUOTE_TTL_SECONDS + 1;
    size_t expired{0};
    BOOST_REQUIRE_MESSAGE(store.ExpireProviderCapacityReservations(expiry, expired, error), error);
    BOOST_CHECK_EQUAL(expired, 0U);
    std::vector<std::pair<uint256, std::vector<unsigned char>>> remaining;
    BOOST_REQUIRE(batch.ListPaymasterCapacityResponses(remaining));
    BOOST_CHECK_EQUAL(remaining.size(), 3U);
    auto& database = GetMockableDatabase(m_wallet);
    const auto after = GetPaymasterCapacityCleanupCursor(database);
    BOOST_REQUIRE(!after.IsNull());
    const auto partially_compacted = database.m_records;
    database.FailWriteAt(0);
    BOOST_CHECK(!store.ExpireProviderCapacityReservations(expiry, expired, error));
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == partially_compacted);
    BOOST_CHECK(GetPaymasterCapacityCleanupCursor(database).IsNull());
    SetPaymasterCapacityCleanupCursor(database, after);
    database.m_pass = false;
    BOOST_CHECK(!store.ExpireProviderCapacityReservations(expiry, expired, error));
    database.m_pass = true;
    BOOST_CHECK(GetPaymasterCapacityCleanupCursor(database).IsNull());

    wallet::CWallet restarted{m_node.chain.get(), "capacity-cleanup-restart",
        DuplicateSecurityFixtureDatabase(database)};
    BOOST_CHECK(GetPaymasterCapacityCleanupCursor(restarted.GetDatabase()).IsNull());
    BOOST_REQUIRE_MESSAGE(PaymasterStore{restarted}.ExpireProviderCapacityReservations(expiry, expired, error), error);
    WalletBatch restarted_batch{restarted.GetDatabase()};
    BOOST_REQUIRE(restarted_batch.ListPaymasterCapacityResponses(remaining));
    BOOST_CHECK(remaining.empty());
    ProviderBudgetLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(ReadProviderSecurityState(restarted, ledger, pool));
    BOOST_CHECK(pool.empty());
    BOOST_CHECK(SerializePaymasterSecurityObject(ledger) == SerializePaymasterSecurityObject(environment.ledger));
    BOOST_REQUIRE(CheckPaymasterCheckpoint(restarted.GetDatabase(), error));
}

namespace {
std::vector<unsigned char> SignClosureUserPSBT(const std::vector<unsigned char>& encoded,
    const CKey& key, std::string& error, size_t input_index = 0)
{
    PartiallySignedTransaction psbt;
    BOOST_REQUIRE(DecodeRawPSBT(psbt, MakeByteSpan(encoded), error));
    BOOST_REQUIRE(psbt.tx);
    const PrecomputedTransactionData txdata = PrecomputePSBTData(psbt);
    ScriptExecutionData execution;
    execution.m_annex_init = true;
    execution.m_annex_present = false;
    uint256 sighash;
    BOOST_REQUIRE(input_index < psbt.inputs.size());
    BOOST_REQUIRE(SignatureHashSchnorr(sighash, execution, CTransaction{*psbt.tx}, input_index,
        SIGHASH_DEFAULT, SigVersion::TAPROOT, txdata, MissingDataBehavior::FAIL));
    std::vector<unsigned char> signature(64);
    const uint256 empty_merkle_root;
    BOOST_REQUIRE(key.SignSchnorr(sighash, signature, &empty_merkle_root, uint256{}));
    psbt.inputs[input_index].final_script_witness.stack = {signature};
    return SerializePaymasterSecurityObject(psbt);
}
} // namespace

BOOST_AUTO_TEST_CASE(provider_unsigned_close_preserves_bindings_and_is_atomic)
{
    constexpr int64_t now{2000};
    auto environment = MakeProviderEnvironment(now, 2000);
    ProviderQuoteFixture first, protected_quote;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(environment,
        "550e8400-e29b-41d4-a716-446655441201", uint256S("2201"), now, first, error,
        nullptr, {}, DGBSatoshis{500}, 5500, Params().GenesisBlock().GetHash()), error);
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(environment,
        "550e8400-e29b-41d4-a716-446655441202", uint256S("2202"), now, protected_quote, error,
        nullptr, {}, DGBSatoshis{500}, 5500, Params().GenesisBlock().GetHash()), error);
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(m_wallet, environment, {first.pool_entry, protected_quote.pool_entry}));
    PaymasterStore store{m_wallet};
    BOOST_REQUIRE_MESSAGE(store.CommitProviderQuote(first.attempt, first.request.intent.genesis_hash, now, error), error);
    BOOST_REQUIRE_MESSAGE(store.CommitProviderQuote(protected_quote.attempt, protected_quote.request.intent.genesis_hash, now, error), error);
    auto& database = GetMockableDatabase(m_wallet);
    const auto before = database.m_records;
    ProviderAttempt closed;
    BOOST_REQUIRE(database.MakeBatch()->Write(DBKeys::PAYMASTER_RESTORE_GUARD, environment.identity));
    const auto quarantined = database.m_records;
    BOOST_CHECK(!store.CancelProviderQuote(first.attempt.attempt_id, now + 1, closed, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    size_t quarantined_expired{0};
    BOOST_CHECK(!store.ExpireProviderQuotes(now + 31, quarantined_expired, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    BOOST_CHECK(database.m_records == quarantined);
    database.m_records = before;
    AlignSecurityFixtureCheckpoint(database);
    // Check every financial row write, the checkpoint write and final commit.
    for (size_t write{0}; write < 5; ++write) {
        database.FailWriteAt(write);
        BOOST_CHECK(!store.CancelProviderQuote(first.attempt.attempt_id, now + 1, closed, error));
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before);
        AlignSecurityFixtureCheckpoint(database);
    }
    database.FailCommit();
    BOOST_CHECK(!store.CancelProviderQuote(first.attempt.attempt_id, now + 1, closed, error));
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before);
    AlignSecurityFixtureCheckpoint(database);
    BOOST_REQUIRE_MESSAGE(store.CancelProviderQuote(first.attempt.attempt_id, now + 1, closed, error), error);
    BOOST_CHECK(closed.state == AttemptState::REJECTED);
    const auto canceled = database.m_records;
    BOOST_REQUIRE_MESSAGE(store.CancelProviderQuote(first.attempt.attempt_id, now + 2, closed, error), error);
    BOOST_CHECK(database.m_records == canceled);
    ProviderBudgetLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(ReadProviderSecurityState(m_wallet, ledger, pool));
    for (const auto& entry : pool) {
        if (entry.outpoint == first.pool_entry.outpoint) {
            BOOST_CHECK(entry.state == PoolEntryState::AVAILABLE);
            BOOST_CHECK(entry.reservation_id.IsNull());
        } else {
            BOOST_CHECK(entry.state == PoolEntryState::RESERVED);
            BOOST_CHECK(entry.reservation_id == protected_quote.attempt.commit_key);
        }
    }
    for (const auto& reservation : ledger.reservations) {
        BOOST_CHECK(reservation.state == (reservation.commit_key == first.attempt.commit_key
            ? BudgetReservationState::RELEASED : BudgetReservationState::RESERVED));
    }

    // The same canonical operation also retains and authenticates a signed
    // negative result; repeated rejection must not charge or release twice.
    PaymasterResult rejection;
    rejection.genesis_hash = Params().GenesisBlock().GetHash();
    rejection.provider_id = environment.identity.provider_id;
    rejection.commit_key = protected_quote.attempt.commit_key;
    rejection.result_sequence = 1;
    rejection.status = PaymasterResultStatus::REJECTED;
    rejection.updated_at = now + 3;
    rejection.identity_signature.resize(64);
    BOOST_REQUIRE(environment.identity_key.SignSchnorr(GetPaymasterResultSignatureHash(rejection),
        rejection.identity_signature, nullptr, uint256{}));
    BOOST_REQUIRE_MESSAGE(store.RejectUnavailableProviderSubmit(protected_quote.attempt.attempt_id, rejection,
        rejection.genesis_hash, closed, error), error);
    const auto rejected = database.m_records;
    BOOST_REQUIRE_MESSAGE(store.RejectUnavailableProviderSubmit(protected_quote.attempt.attempt_id, rejection,
        rejection.genesis_hash, closed, error), error);
    BOOST_REQUIRE_MESSAGE(store.CancelProviderQuote(protected_quote.attempt.attempt_id, now + 4, closed, error), error);
    BOOST_CHECK(database.m_records == rejected);
    auto corrupt = rejection;
    corrupt.identity_signature.front() ^= 1;
    BOOST_REQUIRE(database.MakeBatch()->Write(std::make_pair(DBKeys::PAYMASTER_RESULT, rejection.commit_key), corrupt));
    const auto damaged = database.m_records;
    BOOST_CHECK(!store.CancelProviderQuote(protected_quote.attempt.attempt_id, now + 5, closed, error));
    BOOST_CHECK(database.m_records == damaged);
}

BOOST_AUTO_TEST_CASE(provider_user_only_deadlines_close_and_prune)
{
    constexpr int64_t now{3000};
    for (const bool accepted : {false, true}) {
        auto environment = MakeProviderEnvironment(now, 2000);
        ProviderQuoteFixture fixture;
        std::string error;
        const auto request_id = accepted ? "550e8400-e29b-41d4-a716-446655441204"
                                         : "550e8400-e29b-41d4-a716-446655441203";
        BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(environment, request_id, uint256S(accepted ? "2204" : "2203"),
            now, fixture, error, nullptr, {}, DGBSatoshis{500}, 5500, Params().GenesisBlock().GetHash()), error);
        auto database = CreateMockableWalletDatabase();
        wallet::CWallet provider{m_node.chain.get(), request_id, std::move(database)};
        BOOST_REQUIRE(PersistProviderQuoteEnvironment(provider, environment, {fixture.pool_entry}));
        PaymasterStore store{provider};
        BOOST_REQUIRE_MESSAGE(store.CommitProviderQuote(fixture.attempt, fixture.request.intent.genesis_hash, now, error), error);
        BOOST_REQUIRE_MESSAGE(store.TransitionSession(request_id, SessionState::AUTHORIZED, PendingPhase::NONE, {}, now + 1, error), error);
        ProviderAttempt attempt{fixture.attempt};
        attempt.state = AttemptState::USER_SIGNED;
        attempt.user_signed_psbt = SignClosureUserPSBT(attempt.unsigned_psbt, fixture.user_key, error);
        attempt.updated_at = now + 1;
        BOOST_REQUIRE_MESSAGE(store.UpdateAttempt(request_id, attempt, error), error);
        if (accepted) {
            BOOST_REQUIRE_MESSAGE(store.AcceptUserAuthorization(request_id, attempt.attempt_id,
                Hash(attempt.user_signed_psbt), now + 2, error), error);
        } else {
            // Reproduce the crash/time boundary between the USER-only commit
            // and the first durable provider receipt.
            BOOST_CHECK(!store.AcceptUserAuthorization(request_id, attempt.attempt_id,
                Hash(attempt.user_signed_psbt), attempt.quote_expires_at + 1, error));
        }
        const int64_t deadline = accepted ? attempt.retry_until : attempt.quote_expires_at;
        if (accepted) {
            // Old USER-only owners may outlive the redundant wire cache.
            // Closure must use their authenticated retained quote authority.
            WalletBatch batch{provider.GetDatabase()};
            BOOST_REQUIRE(batch.TxnBegin());
            BOOST_REQUIRE(batch.ErasePaymasterCapacityResponse(fixture.capacity_request_hash));
            BOOST_REQUIRE(batch.ErasePaymasterCapacityNonce(attempt.client_nonce));
            BOOST_REQUIRE(batch.ErasePaymasterCapacitySession(SecurityCapacitySessionKey(fixture.capacity_proof)));
            BOOST_REQUIRE(batch.TxnCommit());
        }
        auto& mock = GetMockableDatabase(provider);
        const auto before = mock.m_records;
        size_t expired{0};
        BOOST_REQUIRE_MESSAGE(store.ExpireProviderQuotes(deadline - 1, expired, error), error);
        BOOST_CHECK_EQUAL(expired, 0U);
        BOOST_CHECK(mock.m_records == before);
        for (const auto* type : {&DBKeys::PAYMASTER_PROVIDER_COMMIT, &DBKeys::PAYMASTER_USER_AUTH, &DBKeys::PAYMASTER_RESULT}) {
            BOOST_REQUIRE(mock.MakeBatch()->Write(std::make_pair(*type, attempt.commit_key), uint16_t{65535}));
            const auto unknown = mock.m_records;
            BOOST_CHECK(!store.ExpireProviderQuotes(deadline + 1, expired, error));
            BOOST_CHECK(mock.m_records == unknown);
            mock.m_records = before;
            AlignSecurityFixtureCheckpoint(mock);
        }
        for (size_t write{0}; write < 5; ++write) {
            mock.FailWriteAt(write);
            BOOST_CHECK(!store.ExpireProviderQuotes(deadline + 1, expired, error));
            mock.ClearFailureInjection();
            BOOST_CHECK(mock.m_records == before);
            AlignSecurityFixtureCheckpoint(mock);
        }
        mock.FailCommit();
        BOOST_CHECK(!store.ExpireProviderQuotes(deadline + 1, expired, error));
        mock.ClearFailureInjection();
        BOOST_CHECK(mock.m_records == before);
        AlignSecurityFixtureCheckpoint(mock);
        BOOST_REQUIRE_MESSAGE(store.ExpireProviderQuotes(deadline + 1, expired, error), error);
        BOOST_CHECK_EQUAL(expired, 1U);
        ProviderAttempt closed;
        BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, closed));
        BOOST_CHECK(closed.state == AttemptState::QUOTE_EXPIRED);
        BOOST_CHECK(closed.user_signed_psbt == attempt.user_signed_psbt);
        bool drain{true};
        BOOST_REQUIRE(store.HasProviderDrainWork(environment.identity.provider_id, drain, error));
        BOOST_CHECK(!drain);
        const auto terminal = mock.m_records;
        BOOST_REQUIRE_MESSAGE(store.CloseExpiredProviderAttempt(attempt.attempt_id, deadline + 2, closed, error), error);
        BOOST_CHECK(mock.m_records == terminal);
        ProviderBudgetLedger ledger;
        std::vector<ProviderPoolEntry> pool;
        BOOST_REQUIRE(ReadProviderSecurityState(provider, ledger, pool));
        BOOST_REQUIRE_EQUAL(pool.size(), 1U);
        BOOST_CHECK(pool.front().state == PoolEntryState::AVAILABLE);
        BOOST_CHECK(pool.front().reservation_id.IsNull());
        BOOST_REQUIRE_EQUAL(ledger.reservations.size(), 1U);
        BOOST_CHECK(ledger.reservations.front().state == BudgetReservationState::RELEASED);
        const int64_t retention = std::max(attempt.retry_until, attempt.capacity_snapshot.expires_at) + 2 +
            paymaster_store::internal::CAPACITY_REPLAY_RETENTION_SECONDS;
        auto restarted_database = DuplicateSecurityFixtureDatabase(provider.GetDatabase());
        wallet::CWallet restarted{m_node.chain.get(), "closed-provider-restart", std::move(restarted_database)};
        PaymasterStore restarted_store{restarted};
        BOOST_REQUIRE_MESSAGE(restarted_store.ReconcileFinalSessionsAtTip(retention, error), error);
        PaymentSession removed;
        BOOST_CHECK(!restarted_store.GetSessionByRequestId(request_id, removed));
        BOOST_CHECK(!restarted_store.GetAttempt(attempt.attempt_id, closed));
        BOOST_REQUIRE(ReadProviderSecurityState(restarted, ledger, pool));
        BOOST_CHECK(pool.front().state == PoolEntryState::AVAILABLE);
    }
}

BOOST_AUTO_TEST_CASE(expired_unsigned_provider_history_is_compacted_atomically)
{
    constexpr int64_t now{1000};
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655441091";
    auto environment = MakeProviderEnvironment(now, /*maximum_reserved=*/2000);
    environment.settings.operation_mode = ProviderOperationMode::MANUAL;
    ProviderQuoteFixture fixture;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(environment, request_id,
        uint256S("1091"), now, fixture, error, nullptr, {}, DGBSatoshis{500},
        5500, Params().GenesisBlock().GetHash()), error);
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(m_wallet, environment, {fixture.pool_entry}));
    PaymasterStore store{m_wallet};
    BOOST_REQUIRE_MESSAGE(store.CommitProviderQuote(fixture.attempt,
        fixture.request.intent.genesis_hash, now, error), error);
    size_t expired{0};
    BOOST_REQUIRE_MESSAGE(store.ExpireProviderQuotes(now + 61, expired, error), error);
    BOOST_REQUIRE_EQUAL(expired, 1U);
    PaymentSession session;
    ProviderAttempt attempt;
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    BOOST_REQUIRE(store.GetAttempt(fixture.attempt.attempt_id, attempt));
    const int64_t boundary = std::max({session.updated_at, attempt.retry_until,
        attempt.capacity_snapshot.expires_at}) + paymaster_store::internal::CAPACITY_REPLAY_RETENTION_SECONDS;
    BOOST_REQUIRE_MESSAGE(store.ReconcileFinalSessionsAtTip(boundary, error), error);
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, attempt));
    auto& database = GetMockableDatabase(m_wallet);
    const auto before = database.m_records;
    database.m_txn_pass = false;
    BOOST_CHECK(!store.ReconcileFinalSessionsAtTip(boundary + 1, error));
    database.m_txn_pass = true;
    BOOST_CHECK(database.m_records == before);
    database.FailCommit();
    BOOST_CHECK(!store.ReconcileFinalSessionsAtTip(boundary + 1, error));
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before);

    AlignSecurityFixtureCheckpoint(database);

    // Even a FAILED/QUOTE_EXPIRED label cannot make signed artifacts prunable.
    ProviderAttempt unsafe{attempt};
    unsafe.user_signed_psbt = {1};
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.WritePaymasterAttempt(unsafe));
    }
    const auto signed_records = database.m_records;
    BOOST_CHECK(!store.ReconcileFinalSessionsAtTip(boundary + 1, error));
    BOOST_CHECK(database.m_records == signed_records);
    database.m_records = before;
    AlignSecurityFixtureCheckpoint(database);

    // Parked restricted history is retained without stopping maintenance.
    unsafe = attempt;
    unsafe.sponsorship_capability_hash = uint256S("1093");
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.WritePaymasterAttempt(unsafe));
    }
    const auto restricted_records = database.m_records;
    BOOST_REQUIRE_MESSAGE(store.ReconcileFinalSessionsAtTip(boundary + 1, error), error);
    BOOST_CHECK(database.m_records == restricted_records);
    database.m_records = before;
    AlignSecurityFixtureCheckpoint(database);

    // A budget/pool binding is independently protective, even if labels lie.
    ProviderBudgetLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(ReadProviderSecurityState(m_wallet, ledger, pool));
    const auto released_ledger = ledger;
    auto row = std::find_if(ledger.reservations.begin(), ledger.reservations.end(),
        [&](const ProviderBudgetReservation& item) { return item.commit_key == attempt.commit_key; });
    if (row != ledger.reservations.end()) row->state = BudgetReservationState::RESERVED;
    else {
        auto bound = environment.ledger;
        BOOST_REQUIRE_MESSAGE(ReserveProviderBudget(bound, environment.safety,
            fixture.request.intent.funding_model, fixture.request.intent.sponsorship_scope,
            attempt.commit_key, fixture.response.quote.network_fee,
            GetRecipientBudgetBucket(bound, fixture.request.intent.recipient_script), now, error,
            environment.netgroup_bucket), error);
        ledger.reservations = bound.reservations;
    }
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{m_wallet.GetDatabase()}.WritePaymasterProviderBudgetLedger(ledger));
    }
    BOOST_CHECK(!store.ReconcileFinalSessionsAtTip(boundary + 1, error));
    BOOST_REQUIRE(store.GetAttempt(attempt.attempt_id, unsafe));
    database.m_records = before;
    AlignSecurityFixtureCheckpoint(database);

    auto restarted_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    wallet::CWallet restarted{m_node.chain.get(), "unsigned-history-restart", std::move(restarted_database)};
    PaymasterStore restarted_store{restarted};
    BOOST_REQUIRE_MESSAGE(restarted_store.ReconcileFinalSessionsAtTip(boundary + 1, error), error);
    BOOST_CHECK(!restarted_store.GetSessionByRequestId(request_id, session));
    BOOST_CHECK(!restarted_store.GetAttempt(attempt.attempt_id, unsafe));
    {
        LOCK(restarted.cs_wallet);
        WalletBatch batch{restarted.GetDatabase()};
        AlignSecurityFixtureCheckpoint(restarted.GetDatabase());
        std::string indexed;
        uint256 indexed_attempt;
        IdempotencyTombstone tombstone;
        BOOST_CHECK(batch.ReadPaymasterSessionIdWithStatus(attempt.session_id, indexed) == DatabaseReadStatus::NOT_FOUND);
        BOOST_CHECK(batch.ReadPaymasterTemplateWithStatus(attempt.template_commitment, indexed_attempt) == DatabaseReadStatus::NOT_FOUND);
        BOOST_CHECK(batch.ReadPaymasterUnsignedTxWithStatus(attempt.unsigned_txid, indexed_attempt) == DatabaseReadStatus::NOT_FOUND);
        BOOST_CHECK(batch.ReadPaymasterTombstoneWithStatus(request_id, tombstone) == DatabaseReadStatus::NOT_FOUND);
    }
    BOOST_REQUIRE(ReadProviderSecurityState(restarted, ledger, pool));
    BOOST_CHECK(SerializePaymasterSecurityObject(ledger) == SerializePaymasterSecurityObject(released_ledger));
    BOOST_CHECK(pool.front().state == PoolEntryState::AVAILABLE);
    const auto compacted = GetMockableDatabase(restarted).m_records;
    BOOST_CHECK(!restarted_store.CommitProviderQuote(fixture.attempt,
        fixture.request.intent.genesis_hash, boundary + 2, error));
    BOOST_CHECK(GetMockableDatabase(restarted).m_records == compacted);
    BOOST_REQUIRE_MESSAGE(restarted_store.ReconcileFinalSessionsAtTip(boundary + 3, error), error);
    BOOST_CHECK(GetMockableDatabase(restarted).m_records == compacted);

    // A delayed submission for the actually pruned template is peer-local;
    // handling it must not recreate authorization or disturb compacted state.
    SyncWithValidationInterfaceQueue();
    {
        LOCK(restarted.cs_wallet);
        restarted.SetLastBlockProcessed(*m_node.chain->getHeight(),
            m_node.chain->getBlockHash(*m_node.chain->getHeight()));
    }
    WalletContext context;
    context.chain = m_node.chain.get();
    context.args = m_node.args;
    Manager manager{true};
    context.paymaster = &manager;
    BOOST_REQUIRE(manager.StartProvider(restarted.GetName(), environment.identity.provider_id));
    PaymasterSubmit delayed;
    delayed.genesis_hash = fixture.request.intent.genesis_hash;
    delayed.request_id = request_id;
    delayed.session_id = attempt.session_id;
    delayed.provider_id = attempt.provider_id;
    delayed.quote_id = attempt.quote_id;
    delayed.commit_key = attempt.commit_key;
    delayed.template_commitment = attempt.template_commitment;
    delayed.user_psbt = {1};
    BOOST_REQUIRE(ValidateSubmitEnvelope(delayed, delayed.genesis_hash, error));
    BOOST_REQUIRE(manager.EnqueueDirectMessage(44, uint256S("1094"), 100,
                                              DirectPayload{delayed}, GetTime()));
    const auto result = paymaster_app::ProcessPaymasterSubmits(
        context, restarted, paymaster_app::ExecutionMode::MANUAL);
    BOOST_CHECK(result.HasMessage());
    BOOST_CHECK(result.IsRejected());
    BOOST_CHECK_EQUAL(result.rejection_reason.value_or(""), "PAYMASTER_SUBMIT_BINDING_MISMATCH");
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
    BOOST_CHECK(manager.IsProviderRunning(restarted.GetName(), environment.identity.provider_id));
    BOOST_CHECK(GetMockableDatabase(restarted).m_records == compacted);
}

BOOST_AUTO_TEST_CASE(provider_history_quota_preserves_existing_requests)
{
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    std::string error;
    PaymentSession session;
    session.provider_side = true;
    session.created_at = session.updated_at = 1000;
    session.canonical_request_hash = uint256S("01");
    for (size_t i = 0; i < paymaster_store::internal::MAX_RETAINED_PROVIDER_SESSIONS; ++i) {
        session.request_id = strprintf("550e8400-e29b-41d4-a716-%012u", i);
        session.session_id = SecurityTestId(uint256S("1092"), i + 1);
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
    }
    BOOST_CHECK(paymaster_store::internal::CheckProviderHistoryCapacity(batch, session.request_id, error));
    BOOST_CHECK(!paymaster_store::internal::CheckProviderHistoryCapacity(batch,
        "550e8400-e29b-41d4-a716-999999999999", error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_HISTORY_FULL");
    BOOST_REQUIRE(batch.ErasePaymasterSession(session.request_id));
    BOOST_CHECK(paymaster_store::internal::CheckProviderHistoryCapacity(batch,
        "550e8400-e29b-41d4-a716-999999999999", error));
}

BOOST_AUTO_TEST_CASE(
    provider_user_authorization_honors_exact_reserved_safety_binding)
{
    constexpr int64_t now{1500};
    constexpr auto request_id =
        "550e8400-e29b-41d4-a716-446655441007";
    ProviderSecurityEnvironment environment =
        MakeProviderEnvironment(now, /*maximum_reserved=*/2000);
    ProviderQuoteFixture fixture;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(
                              environment, request_id, uint256S("1501"), now,
                              fixture, error),
                          error);
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(
        m_wallet, environment, {fixture.pool_entry}));

    PaymasterStore store{m_wallet};
    BOOST_REQUIRE_MESSAGE(
        store.CommitProviderQuote(
            fixture.attempt, fixture.request.intent.genesis_hash, now,
            error),
        error);
    BOOST_REQUIRE_MESSAGE(store.TransitionSession(
                              request_id, SessionState::AUTHORIZED,
                              PendingPhase::NONE, {}, now + 1, error),
                          error);

    ProviderAttempt user_signed{fixture.attempt};
    user_signed.state = AttemptState::USER_SIGNED;
    user_signed.user_signed_psbt = {0x50, 0x4d, 0x41, 0x55, 0x54, 0x48};
    user_signed.updated_at = now + 2;
    BOOST_REQUIRE_MESSAGE(
        store.UpdateAttempt(request_id, user_signed, error), error);
    {
        wallet::CWallet late_wallet{m_node.chain.get(), "late-first-acceptance",
                           DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase())};
        AlignSecurityFixtureCheckpoint(late_wallet.GetDatabase());
        const auto before = GetMockableDatabase(late_wallet).m_records;
        BOOST_CHECK(!PaymasterStore{late_wallet}.AcceptUserAuthorization(request_id,
            user_signed.attempt_id, Hash(user_signed.user_signed_psbt),
            fixture.capacity_proof.expires_at, error));
        BOOST_CHECK(GetMockableDatabase(late_wallet).m_records == before);
    }
    const uint256 user_psbt_hash{Hash(user_signed.user_signed_psbt)};

    // Preserve the exact pre-authorization state for the stale-policy branch.
    auto stale_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    auto stopped_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    auto advertised_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    auto mismatched_budget_database =
        DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    wallet::CWallet stale_wallet{m_node.chain.get(),
                                 "stale-user-authorization-policy",
                                 std::move(stale_database)};
    wallet::CWallet stopped_wallet{m_node.chain.get(),
                                   "stopped-user-authorization-policy",
                                   std::move(stopped_database)};
    wallet::CWallet advertised_wallet{
        m_node.chain.get(), "changed-user-authorization-policy",
        std::move(advertised_database)};
    wallet::CWallet mismatched_budget_wallet{
        m_node.chain.get(), "mismatched-user-authorization-budget",
        std::move(mismatched_budget_database)};

    // The first authorization under the policy that reserved this quote is
    // accepted and becomes durable.
    BOOST_REQUIRE_MESSAGE(store.AcceptUserAuthorization(
                              request_id, user_signed.attempt_id,
                              user_psbt_hash, now + 3, error),
                          error);
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        ProviderSafetyPolicy changed_safety;
        BOOST_REQUIRE(
            batch.ReadPaymasterProviderSafetyPolicy(changed_safety));
        ++changed_safety.maximum_active_quotes_total;
        changed_safety.updated_at = now + 4;
        BOOST_REQUIRE(
            batch.WritePaymasterProviderSafetyPolicy(changed_safety));
        ProviderSettings stopped_settings;
        BOOST_REQUIRE(batch.ReadPaymasterSettings(stopped_settings));
        stopped_settings.enabled = false;
        stopped_settings.updated_at = now + 4;
        BOOST_REQUIRE(batch.WritePaymasterSettings(stopped_settings));
    }
    // A byte-identical replay of the persisted authorization remains
    // drainable after a later operator policy change.
    BOOST_CHECK(store.AcceptUserAuthorization(
        request_id, user_signed.attempt_id, user_psbt_hash, now + 5, error));

    // A quote that already atomically reserved this exact budget remains
    // authorizable after a wallet-local safety-policy update. The historical
    // exception applies only to the policy hash; every reservation and
    // manifest field remains exact.
    {
        LOCK(stale_wallet.cs_wallet);
        WalletBatch batch{stale_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(stale_wallet.GetDatabase());
        ProviderSafetyPolicy changed_safety;
        BOOST_REQUIRE(
            batch.ReadPaymasterProviderSafetyPolicy(changed_safety));
        ++changed_safety.maximum_active_quotes_total;
        changed_safety.updated_at = now + 4;
        BOOST_REQUIRE(
            stale_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_PROVIDER_SAFETY, changed_safety));
    }
    PaymasterStore stale_store{stale_wallet};
    BOOST_REQUIRE_MESSAGE(stale_store.AcceptUserAuthorization(
                              request_id, user_signed.attempt_id,
                              user_psbt_hash, now + 5, error),
                          error);
    UserAuthorizationRecord historical_authorization;
    BOOST_REQUIRE(stale_store.GetUserAuthorization(
        user_signed.commit_key, historical_authorization));
    BOOST_CHECK(historical_authorization.attempt_id ==
                user_signed.attempt_id);
    BOOST_CHECK(historical_authorization.canonical_psbt_hash ==
                user_psbt_hash);
    ProviderAttempt historically_authorized;
    BOOST_REQUIRE(stale_store.GetAttempt(
        user_signed.attempt_id, historically_authorized));
    BOOST_CHECK(historically_authorized.state ==
                AttemptState::USER_PSBT_ACCEPTED);
    PaymentSession historical_session;
    BOOST_REQUIRE(stale_store.GetSessionByRequestId(
        request_id, historical_session));
    BOOST_CHECK(historical_session.state == SessionState::PENDING_PROVIDER);
    BOOST_CHECK(historical_session.pending_phase ==
                PendingPhase::USER_SIGNATURE_SENT);
    ProviderBudgetLedger stale_ledger;
    std::vector<ProviderPoolEntry> stale_pool;
    BOOST_REQUIRE(ReadProviderSecurityState(
        stale_wallet, stale_ledger, stale_pool));
    const ProviderBudgetReservation* stale_reservation =
        FindBudgetReservation(stale_ledger, user_signed.commit_key);
    BOOST_REQUIRE(stale_reservation != nullptr);
    BOOST_CHECK(stale_reservation->state ==
                BudgetReservationState::RESERVED);
    BOOST_REQUIRE_EQUAL(stale_pool.size(), 1U);
    BOOST_CHECK(stale_pool.front().state == PoolEntryState::RESERVED);

    // Historical policy matching never excuses a missing or altered exact
    // reservation. Corrupting its network fee must remain fail-closed and
    // must not persist a user authorization.
    {
        LOCK(mismatched_budget_wallet.cs_wallet);
        WalletBatch batch{mismatched_budget_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(mismatched_budget_wallet.GetDatabase());
        ProviderSafetyPolicy changed_safety;
        ProviderBudgetLedger mismatched_ledger;
        BOOST_REQUIRE(
            batch.ReadPaymasterProviderSafetyPolicy(changed_safety));
        BOOST_REQUIRE(
            batch.ReadPaymasterProviderBudgetLedger(mismatched_ledger));
        ++changed_safety.maximum_active_quotes_total;
        changed_safety.updated_at = now + 4;
        const auto reservation = std::find_if(
            mismatched_ledger.reservations.begin(),
            mismatched_ledger.reservations.end(),
            [&](const ProviderBudgetReservation& entry) {
                return entry.commit_key == user_signed.commit_key;
            });
        BOOST_REQUIRE(reservation != mismatched_ledger.reservations.end());
        ++reservation->network_fee.value;
        BOOST_REQUIRE(
            mismatched_budget_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_PROVIDER_SAFETY, changed_safety));
        BOOST_REQUIRE(
            mismatched_budget_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_PROVIDER_BUDGET, mismatched_ledger));
    }
    PaymasterStore mismatched_budget_store{mismatched_budget_wallet};
    auto& mismatched_budget_mock =
        GetMockableDatabase(mismatched_budget_wallet);
    const MockableData before_mismatched_authorization =
        mismatched_budget_mock.m_records;
    BOOST_CHECK(!mismatched_budget_store.AcceptUserAuthorization(
        request_id, user_signed.attempt_id, user_psbt_hash, now + 5, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PROVIDER_BUDGET_BINDING_MISMATCH");
    BOOST_CHECK(mismatched_budget_mock.m_records ==
                before_mismatched_authorization);
    UserAuthorizationRecord absent;
    BOOST_CHECK(!mismatched_budget_store.GetUserAuthorization(
        user_signed.commit_key, absent));

    // Stopping the provider before the first authorization is independently
    // fail-closed even when the quote's old safety hash still matches.
    {
        LOCK(stopped_wallet.cs_wallet);
        WalletBatch batch{stopped_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(stopped_wallet.GetDatabase());
        ProviderSettings stopped_settings;
        BOOST_REQUIRE(batch.ReadPaymasterSettings(stopped_settings));
        stopped_settings.enabled = false;
        stopped_settings.updated_at = now + 4;
        BOOST_REQUIRE(stopped_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_SETTINGS, stopped_settings));
    }
    PaymasterStore stopped_store{stopped_wallet};
    auto& stopped_mock = GetMockableDatabase(stopped_wallet);
    const MockableData before_stopped_authorization = stopped_mock.m_records;
    BOOST_CHECK(!stopped_store.AcceptUserAuthorization(
        request_id, user_signed.attempt_id, user_psbt_hash, now + 5, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_NOT_RUNNING");
    BOOST_CHECK(stopped_mock.m_records == before_stopped_authorization);

    // A still-enabled provider may change its advertised terms without
    // changing the wallet-local safety limits. The old quote/intent cannot be
    // newly authorized under the replacement policy.
    {
        LOCK(advertised_wallet.cs_wallet);
        WalletBatch batch{advertised_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(advertised_wallet.GetDatabase());
        ProviderPolicy changed_policy;
        ProviderSettings changed_settings;
        BOOST_REQUIRE(batch.ReadPaymasterPolicy(changed_policy));
        BOOST_REQUIRE(batch.ReadPaymasterSettings(changed_settings));
        ++changed_policy.min_payment.value;
        changed_settings.policy_hash =
            GetProviderPolicyHash(changed_policy);
        changed_settings.updated_at = now + 4;
        BOOST_REQUIRE(advertised_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_POLICY, changed_policy));
        BOOST_REQUIRE(advertised_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_SETTINGS, changed_settings));
    }
    PaymasterStore advertised_store{advertised_wallet};
    auto& advertised_mock = GetMockableDatabase(advertised_wallet);
    const MockableData before_advertised_authorization =
        advertised_mock.m_records;
    BOOST_CHECK(!advertised_store.AcceptUserAuthorization(
        request_id, user_signed.attempt_id, user_psbt_hash, now + 5, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_POLICY_NOT_CURRENT");
    BOOST_CHECK(advertised_mock.m_records ==
                before_advertised_authorization);
}

BOOST_AUTO_TEST_CASE(
    provider_pre_signature_firewall_reloads_capacity_pool_and_budget)
{
    constexpr int64_t now{1700};
    constexpr auto request_id =
        "550e8400-e29b-41d4-a716-446655441017";
    ProviderSecurityEnvironment environment =
        MakeProviderEnvironment(now, /*maximum_reserved=*/2000);
    ProviderQuoteFixture fixture;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(
                              environment, request_id, uint256S("1701"), now,
                              fixture, error),
                          error);
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(
        m_wallet, environment, {fixture.pool_entry}));

    PaymasterStore store{m_wallet};
    BOOST_REQUIRE_MESSAGE(store.CommitProviderQuote(
                              fixture.attempt,
                              fixture.request.intent.genesis_hash, now,
                              error),
                          error);
    BOOST_REQUIRE_MESSAGE(store.TransitionSession(
                              request_id, SessionState::AUTHORIZED,
                              PendingPhase::NONE, {}, now + 1, error),
                          error);
    ProviderAttempt user_signed{fixture.attempt};
    user_signed.state = AttemptState::USER_SIGNED;
    user_signed.user_signed_psbt = {0x50, 0x4d, 0x53, 0x49, 0x47};
    user_signed.updated_at = now + 2;
    BOOST_REQUIRE_MESSAGE(
        store.UpdateAttempt(request_id, user_signed, error), error);
    BOOST_REQUIRE_MESSAGE(store.AcceptUserAuthorization(
                              request_id, user_signed.attempt_id,
                              Hash(user_signed.user_signed_psbt), now + 3,
                              error),
                          error);
    ProviderAttempt authorized;
    BOOST_REQUIRE(store.GetAttempt(user_signed.attempt_id, authorized));

    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    auto& mock = GetMockableDatabase(m_wallet);
    const MockableData before_validation = mock.m_records;
    BOOST_REQUIRE_MESSAGE(
        store.ValidateProviderPreSignatureAuthorization(
            authorized, fixture.request.intent.genesis_hash, now + 4,
            capacity_request, capacity_proof, error),
        error);
    BOOST_CHECK(mock.m_records == before_validation);
    BOOST_CHECK(SerializePaymasterSecurityObject(capacity_request) ==
                SerializePaymasterSecurityObject(fixture.capacity_request));
    BOOST_CHECK(SerializePaymasterSecurityObject(capacity_proof) ==
                SerializePaymasterSecurityObject(fixture.capacity_proof));

    // Capacity TTL governs first acceptance. A timely durable receipt permits
    // the exact normal-payment retry, bounded by retry_until and current state.
    BOOST_REQUIRE_MESSAGE(store.ValidateProviderPreSignatureAuthorization(
        authorized, fixture.request.intent.genesis_hash, fixture.capacity_proof.expires_at,
        capacity_request, capacity_proof, error), error);
    BOOST_CHECK(mock.m_records == before_validation);
    BOOST_CHECK(!store.ValidateProviderPreSignatureAuthorization(
        authorized, fixture.request.intent.genesis_hash, authorized.retry_until + 1,
        capacity_request, capacity_proof, error));
    {
        wallet::CWallet invalid_receipt{m_node.chain.get(), "late-durable-receipt",
                               DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase())};
        AlignSecurityFixtureCheckpoint(invalid_receipt.GetDatabase());
        UserAuthorizationRecord receipt;
        BOOST_REQUIRE(PaymasterStore{invalid_receipt}.GetUserAuthorization(authorized.commit_key, receipt));
        receipt.accepted_at = fixture.capacity_proof.expires_at;
        auto raw = invalid_receipt.GetDatabase().MakeBatch();
        BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_USER_AUTH, authorized.commit_key), receipt));
        const auto before = GetMockableDatabase(invalid_receipt).m_records;
        BOOST_CHECK(!PaymasterStore{invalid_receipt}.ValidateProviderPreSignatureAuthorization(
            authorized, fixture.request.intent.genesis_hash, receipt.accepted_at,
            capacity_request, capacity_proof, error));
        BOOST_CHECK(GetMockableDatabase(invalid_receipt).m_records == before);
    }

    auto pool_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    wallet::CWallet pool_wallet{m_node.chain.get(),
                                "pre-signature-pool-conflict",
                                std::move(pool_database)};
    {
        LOCK(pool_wallet.cs_wallet);
        WalletBatch batch{pool_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(pool_wallet.GetDatabase());
        std::vector<ProviderPoolEntry> pool;
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(pool));
        BOOST_REQUIRE_EQUAL(pool.size(), 1U);
        pool.front().reservation_id = SecurityTestId(uint256S("1701"), 99);
        BOOST_REQUIRE(pool_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_PROVIDER_POOL, pool));
    }
    PaymasterStore pool_store{pool_wallet};
    BOOST_CHECK(!pool_store.ValidateProviderPreSignatureAuthorization(
        authorized, fixture.request.intent.genesis_hash, now + 4,
        capacity_request, capacity_proof, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_RESERVATION_CONFLICT");
    BOOST_CHECK(capacity_request.provider_id.IsNull());
    BOOST_CHECK(capacity_proof.provider_id.IsNull());

    auto budget_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    wallet::CWallet budget_wallet{m_node.chain.get(),
                                  "pre-signature-budget-conflict",
                                  std::move(budget_database)};
    {
        LOCK(budget_wallet.cs_wallet);
        WalletBatch batch{budget_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(budget_wallet.GetDatabase());
        ProviderBudgetLedger ledger;
        BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(ledger));
        const auto reservation = std::find_if(
            ledger.reservations.begin(), ledger.reservations.end(),
            [&](const ProviderBudgetReservation& entry) {
                return entry.commit_key == authorized.commit_key;
            });
        BOOST_REQUIRE(reservation != ledger.reservations.end());
        ++reservation->network_fee.value;
        BOOST_REQUIRE(budget_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_PROVIDER_BUDGET, ledger));
    }
    PaymasterStore budget_store{budget_wallet};
    BOOST_CHECK(!budget_store.ValidateProviderPreSignatureAuthorization(
        authorized, fixture.request.intent.genesis_hash, now + 4,
        capacity_request, capacity_proof, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_BUDGET_BINDING_MISMATCH");

    auto capacity_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    wallet::CWallet capacity_wallet{m_node.chain.get(),
                                    "pre-signature-capacity-missing",
                                    std::move(capacity_database)};
    {
        LOCK(capacity_wallet.cs_wallet);
        WalletBatch batch{capacity_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(capacity_wallet.GetDatabase());
        BOOST_REQUIRE(batch.ErasePaymasterCapacityResponse(
            fixture.capacity_request_hash));
    }
    PaymasterStore capacity_store{capacity_wallet};
    const auto capacity_before = GetMockableDatabase(capacity_wallet).m_records;
    BOOST_CHECK(!capacity_store.ValidateProviderPreSignatureAuthorization(
        authorized, fixture.request.intent.genesis_hash, now + 4,
        capacity_request, capacity_proof, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_DATABASE_READ");
    BOOST_CHECK(GetMockableDatabase(capacity_wallet).m_records == capacity_before);
    BOOST_CHECK(SerializePaymasterSecurityObject(capacity_request) ==
                SerializePaymasterSecurityObject(PaymasterCapacityRequest{}));
    BOOST_CHECK(capacity_proof.provider_id.IsNull());
    BOOST_CHECK(capacity_proof.request_id.empty());
    BOOST_CHECK(capacity_proof.liquidity_slots.empty());
    BOOST_CHECK(capacity_proof.identity_signature.empty());

    // Genuine absence has neither index, unlike the dangling response above.
    {
        LOCK(capacity_wallet.cs_wallet);
        WalletBatch batch{capacity_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.ErasePaymasterCapacityNonce(fixture.capacity_request.client_nonce));
        BOOST_REQUIRE(batch.ErasePaymasterCapacitySession(
            paymaster_store::internal::GetPersistedCapacitySessionKey(fixture.capacity_proof)));
    }
    const auto absent_before = GetMockableDatabase(capacity_wallet).m_records;
    BOOST_CHECK(!capacity_store.ValidateProviderPreSignatureAuthorization(
        authorized, fixture.request.intent.genesis_hash, now + 4,
        capacity_request, capacity_proof, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_RESERVATION_MISSING");
    BOOST_CHECK(GetMockableDatabase(capacity_wallet).m_records == absent_before);
    BOOST_CHECK(SerializePaymasterSecurityObject(capacity_request) ==
                SerializePaymasterSecurityObject(PaymasterCapacityRequest{}));
    BOOST_CHECK(capacity_proof.provider_id.IsNull());
    BOOST_CHECK(capacity_proof.request_id.empty());
    BOOST_CHECK(capacity_proof.liquidity_slots.empty());
    BOOST_CHECK(capacity_proof.identity_signature.empty());
}

BOOST_AUTO_TEST_CASE(provider_signing_rejects_maintenance_sources_reused_before_upgrade)
{
    const int64_t now{GetTime()};
    const uint256 seed{uint256S("1739")};
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655441039";
    auto environment = MakeProviderEnvironment(now, 2000);
    ProviderQuoteFixture fixture;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(environment, request_id, seed,
        now, fixture, error, nullptr, {}, DGBSatoshis{500}, 5500,
        Params().GenesisBlock().GetHash()), error);
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(m_wallet, environment, {fixture.pool_entry}));
    BOOST_REQUIRE_MESSAGE(AddTaprootSigningKey(m_wallet, environment.identity_key, error), error);
    BOOST_REQUIRE_MESSAGE(AddTaprootSigningKey(m_wallet, fixture.user_key, error), error);
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(CMutableTransaction{
            fixture.response.quote.reserved_dgb_inputs.front().creating_tx}), TxStateInMempool{}));
        m_wallet.SetLastBlockProcessed(*m_node.chain->getHeight(),
            m_node.chain->getBlockHash(*m_node.chain->getHeight()));
    }
    PaymasterStore store{m_wallet};
    BOOST_REQUIRE_MESSAGE(store.CommitProviderQuote(fixture.attempt,
        fixture.request.intent.genesis_hash, now, error), error);

    CollaborativePSBTTemplate trusted;
    BOOST_REQUIRE(DecodeRawPSBT(trusted.psbt, MakeByteSpan(fixture.attempt.unsigned_psbt), error));
    trusted.input_roles = {InputRole::USER_DD, InputRole::PROVIDER_DGB};
    PartiallySignedTransaction user_psbt{trusted.psbt};
    BOOST_REQUIRE_MESSAGE(SignCollaborativePSBTForParty(m_wallet, user_psbt, trusted,
        SigningParty::USER, error), error);
    BOOST_REQUIRE_MESSAGE(ValidateCollaborativePSBT(user_psbt, trusted,
        CollaborativeSignatureStage::USER_SIGNED, error), error);
    BOOST_REQUIRE_MESSAGE(store.TransitionSession(request_id, SessionState::AUTHORIZED,
        PendingPhase::NONE, {}, now, error), error);
    auto authorized = fixture.attempt;
    authorized.state = AttemptState::USER_SIGNED;
    authorized.user_signed_psbt = SerializePaymasterSecurityObject(user_psbt);
    BOOST_REQUIRE_MESSAGE(store.UpdateAttempt(request_id, authorized, error), error);
    BOOST_REQUIRE_MESSAGE(store.AcceptUserAuthorization(request_id, authorized.attempt_id,
        Hash(authorized.user_signed_psbt), now, error), error);
    BOOST_REQUIRE(store.GetAttempt(authorized.attempt_id, authorized));
    BOOST_REQUIRE(authorized.state == AttemptState::USER_PSBT_ACCEPTED);

    // Reproduce an old wallet that already assigned a retained withdrawal
    // source to this quote. Pool and budget authority alone remain valid, and
    // the chain still contains every input when the upgraded wallet retries.
    {
        LOCK(::cs_main);
        auto& coins = m_node.chainman->ActiveChainstate().CoinsTip();
        for (size_t index = 0; index < trusted.psbt.inputs.size(); ++index) {
            CTxOut prevout;
            BOOST_REQUIRE(trusted.psbt.GetInputUTXO(prevout, index));
            coins.AddCoin(fixture.transaction.vin[index].prevout,
                Coin{std::move(prevout), /*height=*/0, /*coinbase=*/false},
                /*possible_overwrite=*/false);
        }
    }
    PaymasterCapacityRequest capacity_request;
    PaymasterCapacityProof capacity_proof;
    BOOST_REQUIRE_MESSAGE(store.ValidateProviderPreSignatureAuthorization(authorized,
        fixture.request.intent.genesis_hash, now, capacity_request, capacity_proof, error), error);
    BOOST_REQUIRE_MESSAGE(ValidateCapacityProofAgainstChainstate(capacity_proof, capacity_request,
        environment.identity.identity_key, *m_node.chainman, now, error), error);

    ProviderMaintenanceRecord maintenance;
    maintenance.kind = ProviderMaintenanceKind::WITHDRAW_CARRIER_EXCESS;
    maintenance.operation_id = SecurityTestId(seed, 20);
    maintenance.plan_id = SecurityTestId(seed, 21);
    maintenance.state = ProviderMaintenanceState::FAILED;
    maintenance.transaction_id = SecurityTestId(seed, 22);
    maintenance.source_inputs = {fixture.pool_entry.outpoint};
    maintenance.withdrawal_excess_script_pub_key = NewTaprootScript();
    maintenance.withdrawal_excess_amount = DDCents{100};
    maintenance.maximum_fee = DGBSatoshis{2000};
    maintenance.actual_fee = DGBSatoshis{1000};
    maintenance.created_at = maintenance.updated_at = now;
    ProviderMaintenanceOutput successor;
    successor.asset = PoolAsset::DD_CARRIER;
    successor.script_pub_key = NewTaprootScript();
    successor.carrier_value = DDCents{100};
    maintenance.outputs = {successor};
    ProviderMaintenanceLedger ledger;
    ledger.records = {maintenance};
    BOOST_REQUIRE_MESSAGE(ValidateProviderMaintenanceLedger(ledger, error), error);
    WalletBatch batch{m_wallet.GetDatabase()};
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(ledger));
    const auto before = GetMockableDatabase(m_wallet).m_records;
    const auto checkpoint = ReadBinaryFile(PaymasterCheckpointPath(
        fixture.request.intent.genesis_hash, environment.identity.provider_id));
    BOOST_REQUIRE(checkpoint.first);

    // An independent slot stays usable, while an exact retry of the old
    // quote fails locally and does not create a provider signature or commit.
    BOOST_CHECK(CheckPaymasterMaintenanceInputsAvailable(m_wallet,
        {COutPoint{SecurityTestId(seed, 23), 0}}, error));
    // Direct submits participate in named provider work just like the daemon.
    // Reopen the fixture's durable authority under an eligible runtime name.
    wallet::CWallet direct_wallet{m_node.chain.get(), "maintenance-source-submit",
        DuplicateMockDatabase(m_wallet.GetDatabase())};
    BOOST_REQUIRE(direct_wallet.LoadWallet() == DBErrors::LOAD_OK);
    {
        LOCK(direct_wallet.cs_wallet);
        direct_wallet.SetLastBlockProcessed(*m_node.chain->getHeight(),
            m_node.chain->getBlockHash(*m_node.chain->getHeight()));
    }
    Manager direct_manager{true};
    WalletContext direct_context;
    direct_context.paymaster = &direct_manager;
    const auto direct_before = GetMockableDatabase(direct_wallet).m_records;
    for (int retry = 0; retry < 2; ++retry) {
        BOOST_CHECK_EXCEPTION(paymaster_app::SubmitPaymasterTransaction(
            direct_context, direct_wallet, user_psbt, paymaster_app::ExecutionMode::MANUAL), ApplicationError,
            [](const ApplicationError& failure) {
                return failure.kind == ErrorKind::WALLET &&
                    std::string{failure.what()} == "PAYMASTER_MAINTENANCE_SOURCE_CONFLICT";
            });
        BOOST_CHECK(GetMockableDatabase(m_wallet).m_records == before);
        BOOST_CHECK(GetMockableDatabase(direct_wallet).m_records == direct_before);
        BOOST_CHECK(ReadBinaryFile(PaymasterCheckpointPath(
            fixture.request.intent.genesis_hash, environment.identity.provider_id)) == checkpoint);
    }
    ProviderCommitRecord commit;
    BOOST_CHECK(!store.GetProviderCommit(authorized.commit_key, commit));
    ProviderAttempt saved;
    BOOST_REQUIRE(store.GetAttempt(authorized.attempt_id, saved));
    BOOST_CHECK(saved.state == AttemptState::USER_PSBT_ACCEPTED);
    BOOST_CHECK(saved.final_transaction.empty());
    BOOST_CHECK(saved.provider_signed_result.empty());

    // Unreadable or future journal authority also vetoes signing, including
    // otherwise independent inputs. The check never repairs that authority.
    ++ledger.version;
    BOOST_REQUIRE(m_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_MAINTENANCE_LEDGER, ledger));
    const auto unreadable_before = GetMockableDatabase(m_wallet).m_records;
    BOOST_CHECK(!CheckPaymasterMaintenanceInputsAvailable(m_wallet,
        {COutPoint{SecurityTestId(seed, 23), 0}}, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_INVALID_MAINTENANCE_LEDGER");
    BOOST_CHECK(GetMockableDatabase(m_wallet).m_records == unreadable_before);
}

BOOST_AUTO_TEST_CASE(
    normal_submit_classifies_peer_payload_and_local_authority)
{
    const int64_t now{GetTime()};
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655441019";
    const uint256 seed{uint256S("1741")};
    ProviderSecurityEnvironment environment = MakeProviderEnvironment(now, 2000);
    environment.settings.operation_mode = ProviderOperationMode::MANUAL;
    ProviderQuoteFixture fixture;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(
        environment, request_id, seed, now, fixture, error, nullptr, {},
        DGBSatoshis{500}, 5500, Params().GenesisBlock().GetHash()), error);
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(m_wallet, environment, {fixture.pool_entry}));
    PaymasterStore store{m_wallet};
    BOOST_REQUIRE_MESSAGE(store.CommitProviderQuote(
        fixture.attempt, fixture.request.intent.genesis_hash, now, error), error);
    BOOST_REQUIRE_MESSAGE(AddTaprootSigningKey(m_wallet, fixture.user_key, error), error);

    CollaborativePSBTTemplate trusted;
    BOOST_REQUIRE(DecodeRawPSBT(trusted.psbt, MakeByteSpan(fixture.attempt.unsigned_psbt), error));
    trusted.input_roles = {InputRole::USER_DD, InputRole::PROVIDER_DGB};
    BOOST_REQUIRE_MESSAGE(ValidateCollaborativePSBT(trusted.psbt, trusted,
        CollaborativeSignatureStage::UNSIGNED, error), error);
    PartiallySignedTransaction valid_user_psbt{trusted.psbt};
    BOOST_REQUIRE_MESSAGE(SignCollaborativePSBTForParty(
        m_wallet, valid_user_psbt, trusted, SigningParty::USER, error), error);
    BOOST_REQUIRE_MESSAGE(ValidateCollaborativePSBT(valid_user_psbt, trusted,
        CollaborativeSignatureStage::USER_SIGNED, error), error);

    SyncWithValidationInterfaceQueue();
    wallet::CWallet processing_wallet{m_node.chain.get(), "normal-submit-classification",
                                      DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase())};
    {
        LOCK(processing_wallet.cs_wallet);
        processing_wallet.SetLastBlockProcessed(*m_node.chain->getHeight(),
            m_node.chain->getBlockHash(*m_node.chain->getHeight()));
    }
    WalletContext context;
    context.chain = m_node.chain.get();
    context.args = m_node.args;
    PaymasterSubmit submit;
    submit.genesis_hash = fixture.request.intent.genesis_hash;
    submit.request_id = request_id;
    submit.session_id = fixture.attempt.session_id;
    submit.provider_id = fixture.attempt.provider_id;
    submit.quote_id = fixture.attempt.quote_id;
    submit.commit_key = fixture.attempt.commit_key;
    submit.template_commitment = fixture.attempt.template_commitment;
    submit.user_psbt = SerializePaymasterSecurityObject(valid_user_psbt);
    BOOST_REQUIRE(ValidateSubmitEnvelope(submit, submit.genesis_hash, error));
    PaymentSession original_session;
    BOOST_REQUIRE(PaymasterStore{processing_wallet}.GetSessionByRequestId(request_id, original_session));
    const auto attempt_key = std::make_pair(DBKeys::PAYMASTER_ATTEMPT, fixture.attempt.attempt_id);
    const auto session_key = std::make_pair(DBKeys::PAYMASTER_SESSION, std::string{request_id});
    const auto checkpoint_path = PaymasterCheckpointPath(submit.genesis_hash, submit.provider_id);

    uint16_t message_sequence{50};
    const auto check_message = [&](const PaymasterSubmit& incoming,
                                   bool local_failure, const std::string& reason) {
        // Each independent case has a fresh transport replay/rate window.
        // Persisted wallet authority is shared and compared byte for byte.
        Manager manager{true};
        context.paymaster = &manager;
        BOOST_REQUIRE(manager.StartProvider(processing_wallet.GetName(), environment.identity.provider_id));
        const uint256 message_id{SecurityTestId(seed, message_sequence++)};
        BOOST_REQUIRE(manager.EnqueueDirectMessage(43, message_id, 100,
                                                  DirectPayload{incoming}, GetTime()));
        const auto before = GetMockableDatabase(processing_wallet).m_records;
        const auto checkpoint_before = ReadBinaryFile(checkpoint_path);
        BOOST_REQUIRE(checkpoint_before.first);
        try {
            const auto result = paymaster_app::ProcessPaymasterSubmits(
                context, processing_wallet, paymaster_app::ExecutionMode::MANUAL);
            BOOST_CHECK(!local_failure);
            BOOST_CHECK(result.HasMessage());
            BOOST_CHECK(result.IsRejected());
            BOOST_CHECK(!result.queued.value_or(false));
            BOOST_CHECK(!result.rejection_reason.value_or("").empty());
            BOOST_CHECK_EQUAL(result.rejection_reason.value_or(""), reason);
        } catch (const ApplicationError& failure) {
            BOOST_CHECK_MESSAGE(local_failure, failure.what());
            BOOST_CHECK(failure.kind == ErrorKind::WALLET);
            BOOST_CHECK_EQUAL(failure.what(), reason);
        }
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), local_failure ? 1U : 0U);
        BOOST_CHECK(manager.IsProviderRunning(processing_wallet.GetName(), environment.identity.provider_id));
        BOOST_CHECK(GetMockableDatabase(processing_wallet).m_records == before);
        BOOST_CHECK(ReadBinaryFile(checkpoint_path) == checkpoint_before);
        BOOST_REQUIRE(manager.AcknowledgeDirectMessagesIfPresent({message_id}));
    };

    PaymasterSubmit foreign{submit};
    foreign.request_id = "550e8400-e29b-41d4-a716-446655441020";
    check_message(foreign, false, "PAYMASTER_SUBMIT_SESSION_MISMATCH");
    PaymasterSubmit malformed{submit};
    malformed.user_psbt = {1};
    check_message(malformed, false, "PAYMASTER_INVALID_SUBMIT_PSBT");
    PartiallySignedTransaction wrong_signature{valid_user_psbt};
    BOOST_REQUIRE(!wrong_signature.inputs.front().final_script_witness.stack.empty());
    BOOST_REQUIRE(!wrong_signature.inputs.front().final_script_witness.stack.front().empty());
    wrong_signature.inputs.front().final_script_witness.stack.front().front() ^= 1;
    PaymasterSubmit invalid_signature{submit};
    invalid_signature.user_psbt = SerializePaymasterSecurityObject(wrong_signature);
    check_message(invalid_signature, false, "PAYMASTER_PSBT_SIGNATURE_INVALID");

    {
        struct RestoreMockTime {
            const std::chrono::seconds previous{GetMockTime()};
            ~RestoreMockTime() { SetMockTime(previous); }
        } restore_mock_time;
        SetMockTime(fixture.attempt.quote_expires_at + 1);
        check_message(submit, false, "PAYMASTER_SUBMIT_QUOTE_EXPIRED");
        SetMockTime(fixture.attempt.retry_until + 1);
        check_message(submit, false, "PAYMASTER_SUBMIT_RETRY_EXPIRED");

        // Missing is not interchangeable with an unreadable commit. The same
        // otherwise expired peer submit must remain queued for local repair.
        const auto commit_key = std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT, fixture.attempt.commit_key);
        ProviderCommitRecord unsupported;
        ++unsupported.version;
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(commit_key, unsupported));
        check_message(submit, true, "PAYMASTER_SUBMIT_AUTHORIZATION_DATABASE_READ");
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Erase(commit_key));
    }

    BOOST_TEST_CONTEXT("valid divergent user replay versus damaged local signature") {
        // Generate another valid signature for the same transaction with a
        // different auxiliary nonce; mutating a byte would test invalid input.
        PartiallySignedTransaction divergent{valid_user_psbt};
        const PrecomputedTransactionData txdata = PrecomputePSBTData(divergent);
        ScriptExecutionData execution;
        execution.m_annex_init = true;
        execution.m_annex_present = false;
        uint256 sighash;
        const CTransaction transaction{*divergent.tx};
        BOOST_REQUIRE(SignatureHashSchnorr(sighash, execution, transaction, 0,
            SIGHASH_DEFAULT, SigVersion::TAPROOT, txdata, MissingDataBehavior::FAIL));
        const uint256 empty_merkle_root;
        std::vector<unsigned char> signature(64);
        BOOST_REQUIRE(fixture.user_key.SignSchnorr(sighash, signature, &empty_merkle_root, uint256S("1742")));
        divergent.inputs.front().final_script_witness.stack.front() = std::move(signature);
        BOOST_REQUIRE_MESSAGE(ValidateCollaborativePSBT(divergent, trusted,
            CollaborativeSignatureStage::USER_SIGNED, error), error);
        PaymasterSubmit divergent_submit{submit};
        divergent_submit.user_psbt = SerializePaymasterSecurityObject(divergent);
        BOOST_REQUIRE(divergent_submit.user_psbt != submit.user_psbt);

        ProviderAttempt signed_attempt{fixture.attempt};
        signed_attempt.state = AttemptState::USER_SIGNED;
        signed_attempt.user_signed_psbt = submit.user_psbt;
        PaymentSession signed_session{original_session};
        signed_session.state = SessionState::AUTHORIZED;
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(session_key, signed_session));
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, signed_attempt));
        check_message(divergent_submit, false, "PAYMASTER_USER_PSBT_CONFLICT");

        // A complete private provider signature and a durable final commit
        // remain authority after expiry. A different valid USER replay is a
        // peer defect only when that exact local final evidence validates.
        BOOST_REQUIRE_MESSAGE(AddTaprootSigningKey(processing_wallet, environment.identity_key, error), error);
        PartiallySignedTransaction full{valid_user_psbt};
        BOOST_REQUIRE_MESSAGE(SignCollaborativePSBTForParty(processing_wallet, full, trusted,
            SigningParty::PROVIDER, error), error);
        CMutableTransaction final;
        BOOST_REQUIRE(FinalizeAndExtractPSBT(full, final));
        ProviderAttempt provider_signed{signed_attempt};
        provider_signed.state = AttemptState::PROVIDER_SIGNED;
        provider_signed.final_transaction = SerializePaymasterSecurityObject(final);
        provider_signed.final_txid = CTransaction{final}.GetHash();
        provider_signed.provider_signed_at = provider_signed.updated_at = now;
        PaymasterResult final_result;
        final_result.genesis_hash = submit.genesis_hash;
        final_result.provider_id = submit.provider_id;
        final_result.commit_key = submit.commit_key;
        final_result.result_sequence = 1;
        final_result.status = PaymasterResultStatus::FINAL_COMMITTED;
        final_result.txid = provider_signed.final_txid;
        final_result.raw_transaction_hash = CTransaction{final}.GetWitnessHash();
        final_result.final_transaction = final;
        final_result.updated_at = now;
        final_result.identity_signature.resize(64);
        BOOST_REQUIRE(environment.identity_key.SignSchnorr(GetPaymasterResultSignatureHash(final_result),
            final_result.identity_signature, nullptr, uint256{}));
        provider_signed.provider_signed_result = SerializePaymasterSecurityObject(final_result);
        UserAuthorizationRecord authorization;
        authorization.commit_key = submit.commit_key;
        authorization.attempt_id = provider_signed.attempt_id;
        authorization.canonical_psbt_hash = Hash(provider_signed.user_signed_psbt);
        authorization.accepted_at = now;
        authorization.retry_until = provider_signed.retry_until;
        PaymentSession provider_session{signed_session};
        provider_session.state = SessionState::PENDING_PROVIDER;
        provider_session.pending_phase = PendingPhase::PROVIDER_SIGNED_KNOWN;
        const auto authorization_key = std::make_pair(DBKeys::PAYMASTER_USER_AUTH, submit.commit_key);
        const auto result_key = std::make_pair(DBKeys::PAYMASTER_RESULT, submit.commit_key);
        const auto commit_key = std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT, submit.commit_key);
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(authorization_key, authorization));
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(session_key, provider_session));
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, provider_signed));
        struct RestoreMockTime {
            const std::chrono::seconds previous{GetMockTime()};
            ~RestoreMockTime() { SetMockTime(previous); }
        } restore_mock_time;
        SetMockTime(provider_signed.retry_until + 1);
        check_message(divergent_submit, false, "PAYMASTER_USER_PSBT_CONFLICT");
        ProviderAttempt damaged_envelope{provider_signed};
        damaged_envelope.provider_signed_result = {1};
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, damaged_envelope));
        check_message(divergent_submit, true, "PAYMASTER_PROVIDER_SIGNED_RESULT_INVALID");

        ProviderCommitRecord commit;
        commit.commit_key = submit.commit_key;
        commit.provider_id = submit.provider_id;
        commit.quote_id = submit.quote_id;
        commit.template_commitment = submit.template_commitment;
        commit.final_txid = provider_signed.final_txid;
        commit.raw_transaction_hash = *final_result.raw_transaction_hash;
        commit.final_transaction = provider_signed.final_transaction;
        commit.provider_inputs = {fixture.transaction.vin[1].prevout};
        commit.committed_at = now;
        commit.retry_until = provider_signed.retry_until;
        provider_signed.state = AttemptState::FINAL_COMMITTED;
        provider_session.final_txid = provider_signed.final_txid;
        CMutableTransaction validated_final;
        BOOST_REQUIRE_MESSAGE(ValidateProviderCommitForExecution(provider_signed, commit,
            GetTime(), validated_final, error), error);
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(session_key, provider_session));
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, provider_signed));
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(result_key, final_result));
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(commit_key, commit));
        check_message(divergent_submit, false, "PAYMASTER_USER_PSBT_CONFLICT");
        commit.provider_inputs.front() = COutPoint{SecurityTestId(seed, 90), 0};
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(commit_key, commit));
        check_message(divergent_submit, true, "PAYMASTER_PROVIDER_COMMIT_INPUT_MISMATCH");
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Erase(commit_key));
        check_message(divergent_submit, true, "PAYMASTER_SUBMIT_PROVIDER_COMMIT_MISSING");
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Erase(result_key));
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Erase(authorization_key));

        signed_attempt.user_signed_psbt = {1};
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, signed_attempt));
        check_message(submit, true, "PAYMASTER_PERSISTED_USER_PSBT_INVALID");

        ProviderAttempt ambiguous{fixture.attempt};
        ambiguous.state = AttemptState::AMBIGUOUS;
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, ambiguous));
        check_message(submit, true, "PAYMASTER_SUBMIT_AUTHORIZATION_STATE");
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, fixture.attempt));
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(session_key, original_session));
    }

    BOOST_TEST_CONTEXT("local authorization artifact encoding and conflicts") {
        ProviderAttempt corrupt_attempt{fixture.attempt};
        corrupt_attempt.unsigned_psbt = {1};
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, corrupt_attempt));
        check_message(malformed, true, "PAYMASTER_PROVIDER_AUTHORIZATION_TEMPLATE_ENCODING");

        // Changing only the PSBT breaks the signed artifact binding before
        // either the sighash or provider-manifest validator is reached.
        corrupt_attempt = fixture.attempt;
        PartiallySignedTransaction corrupt_template{trusted.psbt};
        corrupt_template.inputs.front().sighash_type = SIGHASH_ALL;
        corrupt_attempt.unsigned_psbt = SerializePaymasterSecurityObject(corrupt_template);
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, corrupt_attempt));
        check_message(submit, true, "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_CONFLICT");

        // An altered local recipient must not turn the unchanged, correctly
        // user-signed payload into a peer rejection.
        corrupt_template = trusted.psbt;
        corrupt_template.tx->vout.front().scriptPubKey = NewTaprootScript();
        corrupt_attempt.unsigned_psbt = SerializePaymasterSecurityObject(corrupt_template);
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, corrupt_attempt));
        check_message(submit, true, "PAYMASTER_PROVIDER_AUTHORIZATION_ARTIFACT_CONFLICT");
    }

    BOOST_TEST_CONTEXT("coherent authorization artifacts with an invalid sighash") {
        CollaborativePSBTTemplate invalid_template{trusted};
        invalid_template.psbt.inputs.front().sighash_type = SIGHASH_ALL;
        PaymasterQuoteResponse response{fixture.response};
        response.quote.template_commitment = GetCollaborativeTemplateCommitment(
            fixture.request.intent, response.quote, invalid_template);
        BOOST_REQUIRE(environment.identity_key.SignSchnorr(
            GetPaymasterQuoteSignatureHash(response.quote), response.quote.identity_signature,
            nullptr, uint256{}));

        ProviderAttempt invalid_attempt{fixture.attempt};
        invalid_attempt.template_commitment = response.quote.template_commitment;
        invalid_attempt.commit_key = GetPaymasterCommitKey(
            invalid_attempt.provider_id, invalid_attempt.client_nonce, invalid_attempt.intent_hash,
            invalid_attempt.quote_id, invalid_attempt.template_commitment);
        invalid_attempt.signed_quote = SerializePaymasterSecurityObject(response);
        invalid_attempt.unsigned_psbt = SerializePaymasterSecurityObject(invalid_template.psbt);
        BOOST_REQUIRE_MESSAGE(BuildProviderAuthorizationManifest(
            fixture.request.intent, response.quote, invalid_template,
            GetProviderSafetyPolicyHash(environment.safety), invalid_attempt.provider_manifest, error), error);

        // Rebind the submit and lookup to the re-signed local artifacts so
        // exact binding checks pass and the forbidden sighash is classified.
        PaymasterSubmit rebound_submit{submit};
        rebound_submit.template_commitment = invalid_attempt.template_commitment;
        rebound_submit.commit_key = invalid_attempt.commit_key;
        BOOST_REQUIRE(ValidateSubmitEnvelope(rebound_submit, rebound_submit.genesis_hash, error));
        const auto template_key = std::make_pair(DBKeys::PAYMASTER_TEMPLATE, invalid_attempt.template_commitment);
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, invalid_attempt));
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(template_key, invalid_attempt.attempt_id));
        check_message(rebound_submit, true, "PAYMASTER_PSBT_SIGHASH");
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Erase(template_key));
    }

    BOOST_TEST_CONTEXT("valid authorization artifacts with a conflicting provider manifest") {
        ProviderAttempt invalid_attempt{fixture.attempt};
        ++invalid_attempt.provider_manifest.maximum_network_fee.value;
        invalid_attempt.provider_manifest.manifest_id =
            GetProviderAuthorizationManifestId(invalid_attempt.provider_manifest);
        PaymasterQuoteRequest decoded_request;
        PaymasterQuoteResponse decoded_response;
        CollaborativePSBTTemplate decoded_template;
        BOOST_REQUIRE_MESSAGE(DecodePaymasterAuthorizationArtifacts(
            invalid_attempt, decoded_request, decoded_response, decoded_template, error), error);
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, invalid_attempt));
        check_message(submit, true, "PAYMASTER_PROVIDER_AUTH_BINDING_MISMATCH");
    }
    BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(attempt_key, fixture.attempt));

    BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Erase(session_key));
    check_message(foreign, true, "PAYMASTER_SUBMIT_SESSION_DATABASE_READ");
    BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(session_key, original_session));
    BOOST_TEST_CONTEXT("authenticated cancellation and signed negative replay") {
        auto& database = GetMockableDatabase(processing_wallet);
        const auto open = database.m_records;
        PaymasterStore processing_store{processing_wallet};
        ProviderAttempt closed;
        BOOST_REQUIRE_MESSAGE(processing_store.CancelProviderQuote(fixture.attempt.attempt_id,
            now, closed, error), error);
        check_message(submit, false, "PAYMASTER_SUBMIT_REJECTED");
        BOOST_REQUIRE(database.MakeBatch()->Write(std::make_pair(DBKeys::PAYMASTER_RESULT,
            fixture.attempt.commit_key), uint16_t{65535}));
        check_message(submit, true, "PAYMASTER_SUBMIT_AUTHORIZATION_DATABASE_READ");
        database.m_records = open;
        AlignSecurityFixtureCheckpoint(database);
        PaymasterResult rejection;
        rejection.genesis_hash = submit.genesis_hash;
        rejection.provider_id = submit.provider_id;
        rejection.commit_key = submit.commit_key;
        rejection.result_sequence = 1;
        rejection.status = PaymasterResultStatus::REJECTED;
        rejection.updated_at = now;
        rejection.identity_signature.resize(64);
        BOOST_REQUIRE(environment.identity_key.SignSchnorr(GetPaymasterResultSignatureHash(rejection),
            rejection.identity_signature, nullptr, uint256{}));
        BOOST_REQUIRE_MESSAGE(processing_store.RejectUnavailableProviderSubmit(fixture.attempt.attempt_id,
            rejection, submit.genesis_hash, closed, error), error);
        const auto rejected = database.m_records;
        Manager manager{true};
        context.paymaster = &manager;
        BOOST_REQUIRE(manager.StartProvider(processing_wallet.GetName(), submit.provider_id));
        BOOST_REQUIRE(manager.EnqueueDirectMessage(43, SecurityTestId(seed, message_sequence++), 100,
            DirectPayload{submit}, now));
        const auto replay = paymaster_app::ProcessPaymasterSubmits(context, processing_wallet,
            paymaster_app::ExecutionMode::MANUAL);
        BOOST_CHECK(replay.queued.value_or(false));
        BOOST_CHECK(replay.result_status == PaymasterResultStatus::REJECTED);
        const auto outbound = manager.TakeOutboundDirectMessages(43, 1, now);
        BOOST_REQUIRE_EQUAL(outbound.size(), 1U);
        const auto* message = std::get_if<PaymasterResultMessage>(&outbound.front().payload);
        BOOST_REQUIRE(message);
        BOOST_CHECK(SerializePaymasterSecurityObject(message->result) == SerializePaymasterSecurityObject(rejection));
        BOOST_CHECK(database.m_records == rejected);
        rejection.identity_signature.front() ^= 1;
        BOOST_REQUIRE(database.MakeBatch()->Write(std::make_pair(DBKeys::PAYMASTER_RESULT,
            rejection.commit_key), rejection));
        check_message(submit, true, "PAYMASTER_INVALID_RESULT_SIGNATURE");
        database.m_records = open;
        AlignSecurityFixtureCheckpoint(database);
    }
    {
        struct RestoreMockTime {
            const std::chrono::seconds previous{GetMockTime()};
            ~RestoreMockTime() { SetMockTime(previous); }
        } restore_mock_time;
        SetMockTime(fixture.attempt.quote_expires_at + 1);
        size_t expired{0};
        BOOST_REQUIRE_MESSAGE(PaymasterStore{processing_wallet}.ExpireProviderQuotes(GetTime(), expired, error), error);
        BOOST_REQUIRE_EQUAL(expired, 1U);
        check_message(submit, false, "PAYMASTER_SUBMIT_QUOTE_EXPIRED");
    }
}

namespace {
struct ProviderRecoveryQuoteFixture {
    ProviderSecurityEnvironment environment;
    ProviderPoolEntry pool_entry;
    AlternativeRecoveryRecord recovery;
    CKey user_key;
    CTransactionRef user_transaction;
};

ProviderRecoveryQuoteFixture MakeProviderRecoveryQuoteFixture(
    int64_t now, const char* request_id, const uint256& seed,
    const ProviderSecurityEnvironment* inherited = nullptr)
{
    const uint256 genesis_hash{Params().GenesisBlock().GetHash()};
    ProviderSecurityEnvironment environment = inherited ? *inherited :
        MakeProviderEnvironment(now, /*maximum_reserved=*/2000);
    environment.policy.funding_models = FUNDING_MODEL_USER_PAID;
    environment.settings.policy_hash =
        GetProviderPolicyHash(environment.policy);
    environment.safety.user_paid = SecurityLimits(/*maximum_reserved=*/2000);

    CMutableTransaction provider_previous;
    provider_previous.vin.emplace_back(
        COutPoint{SecurityTestId(seed, 2), 0});
    provider_previous.vout.emplace_back(
        2000, environment.identity.identity_script);
    VerifiedDGBInput provider_input;
    provider_input.outpoint =
        COutPoint{CTransaction{provider_previous}.GetHash(), 0};
    provider_input.creating_tx = provider_previous;
    provider_input.value = DGBSatoshis{2000};

    ProviderPoolEntry pool_entry;
    pool_entry.outpoint = provider_input.outpoint;
    pool_entry.purpose = PoolPurpose::OPERATIONAL;
    pool_entry.asset = PoolAsset::DGB;
    pool_entry.state = PoolEntryState::RESERVED;
    pool_entry.script_pub_key =
        provider_previous.vout.front().scriptPubKey;
    pool_entry.dgb_value = provider_input.value;
    pool_entry.confirmation_height = 1;
    pool_entry.reservation_id = SecurityTestId(seed, 3);
    pool_entry.updated_at = now - 1;

    PaymasterCapacityRequest capacity_request;
    capacity_request.genesis_hash = genesis_hash;
    capacity_request.provider_id = environment.identity.provider_id;
    capacity_request.request_id = request_id;
    capacity_request.session_id = SecurityTestId(seed, 4);
    capacity_request.client_nonce = pool_entry.reservation_id;
    capacity_request.funding_model = FundingModel::USER_PAID;
    capacity_request.requires_carrier = false;
    capacity_request.requested_slots = 1;
    capacity_request.created_at = now - 2;
    capacity_request.expires_at = now + 30;

    PaymasterCapacityProof capacity_proof;
    capacity_proof.genesis_hash = genesis_hash;
    capacity_proof.provider_id = environment.identity.provider_id;
    capacity_proof.request_id = request_id;
    capacity_proof.session_id = capacity_request.session_id;
    capacity_proof.client_nonce = capacity_request.client_nonce;
    capacity_proof.funding_model = FundingModel::USER_PAID;
    capacity_proof.requires_carrier = false;
    capacity_proof.created_at = capacity_request.created_at;
    capacity_proof.expires_at = capacity_request.expires_at;
    CapacityDGBInput capacity_input;
    capacity_input.input = provider_input;
    capacity_input.control_proof.reference_block = genesis_hash;
    capacity_input.control_proof.expires_at = capacity_proof.expires_at;
    PaymasterLiquiditySlot slot;
    slot.dgb_inputs = {capacity_input};
    capacity_proof.liquidity_slots = {slot};
    capacity_proof.snapshot_id =
        SecurityCapacitySnapshotId(capacity_proof);
    CapacityDGBInput& signed_capacity_input =
        capacity_proof.liquidity_slots.front().dgb_inputs.front();
    signed_capacity_input.control_proof.signature.resize(64);
    const uint256 empty_merkle_root;
    BOOST_REQUIRE(environment.identity_key.SignSchnorr(
        GetCapacityControlHash(
            capacity_proof, signed_capacity_input.input.outpoint,
            signed_capacity_input.control_proof.expires_at),
        signed_capacity_input.control_proof.signature, &empty_merkle_root,
        uint256{}));
    capacity_proof.identity_signature.resize(64);
    BOOST_REQUIRE(environment.identity_key.SignSchnorr(
        GetCapacityProofSignatureHash(capacity_proof),
        capacity_proof.identity_signature, nullptr, uint256{}));
    const std::vector<unsigned char> encoded_capacity_proof =
        SerializePaymasterSecurityObject(capacity_proof);
    const uint256 capacity_request_hash{
        Hash(SerializePaymasterSecurityObject(capacity_request))};
    const uint256 request_key = GetProviderRequestSlotKey(
        capacity_request.provider_id, capacity_request.request_id,
        capacity_request.session_id);
    std::string error;
    BOOST_REQUIRE_MESSAGE(ReserveProviderCapacityAdmission(
                              environment.ledger, environment.safety,
                              request_key, capacity_request_hash,
                              environment.netgroup_bucket,
                              FundingModel::USER_PAID,
                              /*requires_carrier=*/false,
                              capacity_request.expires_at, now - 2, error),
                          error);
    environment.capacity_records.push_back(
        {capacity_request.client_nonce, capacity_request_hash,
         SecurityCapacitySessionKey(capacity_proof),
         encoded_capacity_proof});

    AlternativeRecoveryRecord recovery;
    recovery.provider_side = true;
    recovery.request_id = request_id;
    recovery.session_id = capacity_request.session_id;
    recovery.original_provider_id = SecurityTestId(seed, 5);
    recovery.recovery_provider_id = environment.identity.provider_id;
    recovery.offer_id = SecurityTestId(seed, 6);
    recovery.policy_hash = environment.settings.policy_hash;
    recovery.recovery_provider_identity_key =
        environment.identity.identity_key;
    recovery.recovery_provider_endpoint = "127.0.0.1:18444";
    recovery.original_commit_key = SecurityTestId(seed, 7);
    recovery.original_template_commitment = SecurityTestId(seed, 8);
    recovery.selected_maximum_service_fee = DDCents{100};
    recovery.selected_service_fee = DDCents{100};
    recovery.client_nonce = capacity_request.client_nonce;
    recovery.capacity_request = capacity_request;
    recovery.recovery_id = GetAlternativeRecoveryId(
        recovery.request_id, recovery.session_id,
        recovery.recovery_provider_id, recovery.client_nonce);
    recovery.capacity_snapshot.snapshot_id = capacity_proof.snapshot_id;
    recovery.capacity_snapshot.resource_commitment =
        GetCapacityResourceCommitment(capacity_proof);
    recovery.capacity_snapshot.session_id = recovery.session_id;
    recovery.capacity_snapshot.attempt_id = recovery.recovery_id;
    recovery.capacity_snapshot.provider_id = recovery.recovery_provider_id;
    recovery.capacity_snapshot.client_nonce = recovery.client_nonce;
    recovery.capacity_snapshot.request_hash = capacity_request_hash;
    recovery.capacity_snapshot.capacity_proof = encoded_capacity_proof;
    recovery.capacity_snapshot.created_at = capacity_proof.created_at;
    recovery.capacity_snapshot.expires_at = capacity_proof.expires_at;
    recovery.capacity_snapshot.validated_at = now - 1;
    recovery.capacity_snapshot.funding_model = FundingModel::USER_PAID;
    recovery.capacity_snapshot.requires_carrier = false;

    AlternativeRecoveryRequest& request = recovery.recovery_request;
    request.genesis_hash = genesis_hash;
    request.request_id = recovery.request_id;
    request.session_id = recovery.session_id;
    request.original_provider_id = recovery.original_provider_id;
    request.recovery_provider_id = recovery.recovery_provider_id;
    request.offer_id = recovery.offer_id;
    request.policy_hash = recovery.policy_hash;
    request.original_commit_key = recovery.original_commit_key;
    request.original_template_commitment =
        recovery.original_template_commitment;
    request.client_nonce = recovery.client_nonce;
    request.capacity_request = capacity_request;
    request.capacity_snapshot_id = recovery.capacity_snapshot.snapshot_id;
    request.capacity_resource_commitment =
        recovery.capacity_snapshot.resource_commitment;
    request.user_dd_inputs = {COutPoint{SecurityTestId(seed, 9), 0}};
    request.wallet_returns = {{NewTaprootScript(), DDCents{900}}};
    request.maximum_service_fee = recovery.selected_maximum_service_fee;
    request.service_fee = recovery.selected_service_fee;
    request.created_at = now - 1;
    request.expires_at = now + 25;
    request.user_input_proofs = {
        {request.user_dd_inputs.front(), std::vector<unsigned char>(64, 1)}};
    recovery.recovery_request_hash =
        GetAlternativeRecoveryRequestHash(request);

    AlternativeRecoveryResponse& response = recovery.recovery_response;
    response.genesis_hash = genesis_hash;
    response.request_id = recovery.request_id;
    response.session_id = recovery.session_id;
    response.recovery_id = recovery.recovery_id;
    response.recovery_provider_id = recovery.recovery_provider_id;
    response.recovery_request_hash = recovery.recovery_request_hash;
    AlternativeRecoveryManifest& manifest = response.manifest;
    manifest.request_id = recovery.request_id;
    manifest.session_id = recovery.session_id;
    manifest.original_provider_id = recovery.original_provider_id;
    manifest.recovery_provider_id = recovery.recovery_provider_id;
    manifest.offer_id = recovery.offer_id;
    manifest.policy_hash = recovery.policy_hash;
    manifest.original_commit_key = recovery.original_commit_key;
    manifest.original_template_commitment =
        recovery.original_template_commitment;
    manifest.capacity_snapshot_id = request.capacity_snapshot_id;
    manifest.capacity_resource_commitment =
        request.capacity_resource_commitment;
    manifest.user_dd_inputs = request.user_dd_inputs;
    manifest.wallet_returns = request.wallet_returns;
    manifest.recovery_provider_dgb_inputs = {provider_input.outpoint};
    manifest.recovery_provider_dd_scripts = {
        environment.identity.identity_script};
    manifest.recovery_provider_dgb_change_scripts = {
        environment.identity.identity_script};
    manifest.maximum_service_fee = request.maximum_service_fee;
    manifest.service_fee = request.service_fee;
    manifest.network_fee = DGBSatoshis{500};
    manifest.expires_at = now + 20;
    manifest.unsigned_txid = SecurityTestId(seed, 10);
    manifest.template_commitment = SecurityTestId(seed, 11);
    manifest.manifest_id = GetAlternativeRecoveryManifestId(manifest);
    response.unsigned_psbt = {0x01};
    response.created_at = now;
    response.expires_at = manifest.expires_at;
    response.recovery_commit_key =
        GetAlternativeRecoveryCommitKey(response);
    response.identity_signature.resize(64);
    BOOST_REQUIRE(environment.identity_key.SignSchnorr(
        GetAlternativeRecoveryResponseSignatureHash(response),
        response.identity_signature, nullptr, uint256{}));
    BOOST_REQUIRE_MESSAGE(BuildRecoveryAuthorizationManifest(
                              request, response,
                              recovery.recovery_authorization, error),
                          error);
    recovery.phase = AlternativeRecoveryPhase::RESPONSE_VALIDATED;
    recovery.created_at = now;
    recovery.updated_at = now;
    recovery.provider_safety_policy_hash =
        GetProviderSafetyPolicyHash(environment.safety);
    recovery.provider_budget_reservation_id =
        response.recovery_commit_key;
    recovery.provider_netgroup_bucket = environment.netgroup_bucket;
    recovery.provider_maximum_network_fee = manifest.network_fee;

    return {std::move(environment), std::move(pool_entry), std::move(recovery)};
}

ProviderRecoveryQuoteFixture MakeSignableProviderRecoveryQuoteFixture(
    int64_t now, const char* request_id, const uint256& seed)
{
    auto fixture = MakeProviderRecoveryQuoteFixture(now, request_id, seed);
    fixture.user_key.MakeNewKey(true);
    const auto output_key = XOnlyPubKey{fixture.user_key.GetPubKey()}.CreateTapTweak(nullptr);
    BOOST_REQUIRE(output_key);
    const CScript user_script = GetScriptForDestination(WitnessV1Taproot{output_key->first});
    CMutableTransaction previous;
    previous.vin.emplace_back(COutPoint{SecurityTestId(seed, 21), 0});
    previous.vout.emplace_back(0, user_script);
    fixture.user_transaction = MakeTransactionRef(std::move(previous));
    auto& recovery = fixture.recovery;
    auto& request = recovery.recovery_request;
    auto& response = recovery.recovery_response;
    auto& manifest = response.manifest;
    request.user_dd_inputs = {COutPoint{fixture.user_transaction->GetHash(), 0}};
    request.user_input_proofs = {{request.user_dd_inputs.front(), std::vector<unsigned char>(64)}};
    const uint256 empty_merkle_root;
    BOOST_REQUIRE(fixture.user_key.SignSchnorr(GetAlternativeRecoveryInputControlHash(request,
        request.user_dd_inputs.front()), request.user_input_proofs.front().signature, &empty_merkle_root, uint256{}));
    recovery.recovery_request_hash = GetAlternativeRecoveryRequestHash(request);
    response.recovery_request_hash = recovery.recovery_request_hash;
    manifest.user_dd_inputs = request.user_dd_inputs;
    PaymasterCapacityProof proof;
    std::string error;
    BOOST_REQUIRE(paymaster_store::internal::DecodeCanonicalCapacityProof(recovery.capacity_snapshot.capacity_proof, proof));
    const auto& funding = proof.liquidity_slots.front().dgb_inputs.front().input;
    CMutableTransaction transaction;
    transaction.vin.emplace_back(request.user_dd_inputs.front());
    transaction.vin.emplace_back(funding.outpoint);
    transaction.vout.emplace_back(0, manifest.wallet_returns.front().script_pub_key);
    transaction.vout.emplace_back(0, manifest.recovery_provider_dd_scripts.front());
    transaction.vout.emplace_back(funding.value.value - manifest.network_fee.value,
        manifest.recovery_provider_dgb_change_scripts.front());
    CollaborativePSBTTemplate trusted;
    BOOST_REQUIRE_MESSAGE(CreateCollaborativePSBTTemplate(transaction,
        {{request.user_dd_inputs.front(), fixture.user_transaction, InputRole::USER_DD},
         {funding.outpoint, MakeTransactionRef(CMutableTransaction{funding.creating_tx}), InputRole::PROVIDER_DGB}},
        trusted, error), error);
    manifest.unsigned_txid = CTransaction{transaction}.GetHash();
    manifest.template_commitment = GetAlternativeRecoveryTemplateCommitment(manifest, trusted);
    manifest.manifest_id = GetAlternativeRecoveryManifestId(manifest);
    response.unsigned_psbt = SerializePaymasterSecurityObject(trusted.psbt);
    response.recovery_commit_key = GetAlternativeRecoveryCommitKey(response);
    BOOST_REQUIRE(fixture.environment.identity_key.SignSchnorr(GetAlternativeRecoveryResponseSignatureHash(response),
        response.identity_signature, nullptr, uint256{}));
    BOOST_REQUIRE_MESSAGE(BuildRecoveryAuthorizationManifest(request, response, recovery.recovery_authorization, error), error);
    recovery.provider_budget_reservation_id = response.recovery_commit_key;
    return fixture;
}
} // namespace

BOOST_AUTO_TEST_CASE(provider_user_only_recovery_expiry_is_atomic_and_cannot_resurrect)
{
    constexpr int64_t now{4000};
    auto fixture = MakeSignableProviderRecoveryQuoteFixture(now,
        "550e8400-e29b-41d4-a716-446655441205", uint256S("2205"));
    auto& recovery = fixture.recovery;
    auto& environment = fixture.environment;
    std::string error;
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(m_wallet, environment, {fixture.pool_entry}));
    PaymasterStore store{m_wallet};
    BOOST_REQUIRE_MESSAGE(store.CommitProviderAlternativeRecoveryQuote(recovery, environment.netgroup_bucket,
        Params().GenesisBlock().GetHash(), now, error), error);
    recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
    recovery.user_signed_psbt = SignClosureUserPSBT(recovery.recovery_response.unsigned_psbt, fixture.user_key, error);
    recovery.updated_at = now + 1;
    BOOST_REQUIRE_MESSAGE(store.UpdateAlternativeRecovery(recovery, error), error);
    BOOST_REQUIRE_MESSAGE(store.ValidateProviderAlternativeRecoveryPreSignatureAuthorization(recovery,
        Params().GenesisBlock().GetHash(), now + 2, error), error);
    const int64_t deadline = std::min(recovery.recovery_response.expires_at, recovery.capacity_snapshot.expires_at);
    auto& database = GetMockableDatabase(m_wallet);
    const auto before = database.m_records;
    size_t expired{0};
    BOOST_REQUIRE_MESSAGE(store.ExpireAlternativeRecoveries(deadline - 1, expired, error), error);
    BOOST_CHECK_EQUAL(expired, 0U);
    BOOST_CHECK(database.m_records == before);
    const auto& key = recovery.recovery_response.recovery_commit_key;
    BOOST_REQUIRE(database.MakeBatch()->Write(DBKeys::PAYMASTER_RESTORE_GUARD, environment.identity));
    const auto quarantined = database.m_records;
    BOOST_CHECK(!store.CloseExpiredProviderRecovery(recovery.recovery_id, deadline, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    BOOST_CHECK(!store.ExpireAlternativeRecoveries(deadline, expired, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_RESTORE_REVIEW_REQUIRED");
    BOOST_CHECK(database.m_records == quarantined);
    database.m_records = before;
    AlignSecurityFixtureCheckpoint(database);
    for (const auto* type : {&DBKeys::PAYMASTER_PROVIDER_COMMIT, &DBKeys::PAYMASTER_USER_AUTH, &DBKeys::PAYMASTER_RESULT}) {
        BOOST_REQUIRE(database.MakeBatch()->Write(std::make_pair(*type, key), uint16_t{65535}));
        const auto unknown = database.m_records;
        BOOST_CHECK(!store.ExpireAlternativeRecoveries(deadline, expired, error));
        BOOST_CHECK(database.m_records == unknown);
        database.m_records = before;
        AlignSecurityFixtureCheckpoint(database);
    }
    for (size_t write{0}; write < 4; ++write) {
        database.FailWriteAt(write);
        BOOST_CHECK(!store.ExpireAlternativeRecoveries(deadline, expired, error));
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before);
        AlignSecurityFixtureCheckpoint(database);
    }
    database.FailCommit();
    BOOST_CHECK(!store.ExpireAlternativeRecoveries(deadline, expired, error));
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before);
    AlignSecurityFixtureCheckpoint(database);

    // A provider may already have computed a private final artifact when
    // periodic expiry wins the wallet lock, before durable preflight/commit.
    const auto final_psbt_bytes = SignClosureUserPSBT(recovery.user_signed_psbt, environment.identity_key, error, 1);
    PartiallySignedTransaction final_psbt;
    BOOST_REQUIRE(DecodeRawPSBT(final_psbt, MakeByteSpan(final_psbt_bytes), error));
    CMutableTransaction final;
    BOOST_REQUIRE(FinalizeAndExtractPSBT(final_psbt, final));
    AlternativeRecoveryRecord stale_final{recovery};
    stale_final.phase = AlternativeRecoveryPhase::FINAL_COMMITTED;
    stale_final.final_transaction = SerializePaymasterSecurityObject(final);
    stale_final.expected_wtxid = CTransaction{final}.GetWitnessHash();
    stale_final.updated_at = deadline;
    auto& result = stale_final.signed_result;
    result.genesis_hash = Params().GenesisBlock().GetHash();
    result.provider_id = environment.identity.provider_id;
    result.commit_key = key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::FINAL_COMMITTED;
    result.txid = CTransaction{final}.GetHash();
    result.raw_transaction_hash = Hash(stale_final.final_transaction);
    result.final_transaction = final;
    result.updated_at = deadline;
    result.identity_signature.resize(64);
    BOOST_REQUIRE(environment.identity_key.SignSchnorr(GetPaymasterResultSignatureHash(result),
        result.identity_signature, nullptr, uint256{}));
    ProviderCommitRecord commit;
    commit.commit_key = key;
    commit.provider_id = result.provider_id;
    commit.quote_id = recovery.recovery_id;
    commit.template_commitment = recovery.recovery_response.manifest.template_commitment;
    commit.final_txid = *result.txid;
    commit.raw_transaction_hash = *result.raw_transaction_hash;
    commit.final_transaction = stale_final.final_transaction;
    commit.provider_inputs = {fixture.pool_entry.outpoint};
    commit.committed_at = deadline;
    BOOST_REQUIRE_MESSAGE(AddTaprootSigningKey(m_wallet, environment.identity_key, error), error);
    PaymasterCapacityProof proof;
    BOOST_REQUIRE(paymaster_store::internal::DecodeCanonicalCapacityProof(recovery.capacity_snapshot.capacity_proof, proof));
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.AddToWallet(fixture.user_transaction, TxStateInactive{}));
        BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(CMutableTransaction{
            proof.liquidity_slots.front().dgb_inputs.front().input.creating_tx}), TxStateInactive{}));
    }
    BOOST_REQUIRE_MESSAGE(store.ExpireAlternativeRecoveries(deadline, expired, error), error);
    BOOST_CHECK_EQUAL(expired, 1U);
    AlternativeRecoveryRecord closed;
    BOOST_REQUIRE(store.GetAlternativeRecoveryById(recovery.recovery_id, closed));
    BOOST_CHECK(closed.expired);
    BOOST_CHECK(closed.phase == AlternativeRecoveryPhase::USER_SIGNED);
    BOOST_CHECK(closed.user_signed_psbt == recovery.user_signed_psbt);
    const auto terminal = database.m_records;
    BOOST_REQUIRE_MESSAGE(store.CloseExpiredProviderRecovery(recovery.recovery_id, deadline + 1, error), error);
    BOOST_CHECK(!store.UpdateAlternativeRecovery(stale_final, error));
    BOOST_CHECK(!store.ValidateProviderAlternativeRecoveryPreSignatureAuthorization(recovery,
        Params().GenesisBlock().GetHash(), deadline, error));
    BOOST_CHECK(!store.CommitProviderAlternativeRecoveryFinal(stale_final, commit, result,
        Params().GenesisBlock().GetHash(), error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_ALTERNATIVE_RECOVERY_EXPIRED");
    BOOST_CHECK(database.m_records == terminal);
    ProviderBudgetLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(ReadProviderSecurityState(m_wallet, ledger, pool));
    BOOST_REQUIRE_EQUAL(pool.size(), 1U);
    BOOST_CHECK(pool.front().state == PoolEntryState::AVAILABLE);
    BOOST_CHECK(pool.front().reservation_id.IsNull());
    BOOST_REQUIRE_EQUAL(ledger.reservations.size(), 1U);
    BOOST_CHECK(ledger.reservations.front().state == BudgetReservationState::RELEASED);
    bool drain{true};
    BOOST_REQUIRE(store.HasProviderDrainWork(environment.identity.provider_id, drain, error));
    BOOST_CHECK(!drain);
    wallet::CWallet restarted{m_node.chain.get(), "closed-recovery-restart", DuplicateSecurityFixtureDatabase(database)};
    const int64_t prune_at = recovery.capacity_request.expires_at + 1 + paymaster_store::internal::CAPACITY_REPLAY_RETENTION_SECONDS;
    BOOST_REQUIRE_MESSAGE(PaymasterStore{restarted}.ExpireAlternativeRecoveries(prune_at, expired, error), error);
    BOOST_CHECK(!PaymasterStore{restarted}.GetAlternativeRecoveryById(recovery.recovery_id, closed));
    BOOST_REQUIRE(CheckPaymasterCheckpoint(restarted.GetDatabase(), error));
}

BOOST_AUTO_TEST_CASE(provider_history_quota_counts_alternative_recovery_quotes)
{
    constexpr int64_t now{1750};
    auto fixture = MakeProviderRecoveryQuoteFixture(now,
        "550e8400-e29b-41d4-a716-446655441118", uint256S("1851"));
    auto& environment = fixture.environment;
    const std::vector<unsigned char> canonical_netgroup{1, 2, 3};
    environment.netgroup_bucket = GetNetgroupBudgetBucket(environment.ledger, canonical_netgroup);
    for (auto& entry : environment.ledger.capacity_admissions) entry.netgroup_bucket = environment.netgroup_bucket;
    fixture.recovery.provider_netgroup_bucket = environment.netgroup_bucket;
    const auto& recovery = fixture.recovery;
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(m_wallet, environment, {fixture.pool_entry}));
    LOCK(m_wallet.cs_wallet);
    WalletBatch batch{m_wallet.GetDatabase()};
    PaymentSession session;
    session.provider_side = true;
    session.created_at = session.updated_at = now;
    session.canonical_request_hash = uint256::ONE;
    BOOST_REQUIRE(batch.TxnBegin());
    for (size_t i = 0; i < paymaster_store::internal::MAX_RETAINED_PROVIDER_SESSIONS; ++i) {
        session.request_id = strprintf("550e8400-e29b-41d4-a716-%012u", i);
        session.session_id = SecurityTestId(uint256S("1852"), i + 1);
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
    }
    BOOST_REQUIRE(batch.TxnCommit());
    const auto full = GetMockableDatabase(m_wallet).m_records;
    PaymasterStore store{m_wallet};
    std::string error;
    uint256 netgroup;
    const auto admission = [&] {
        return store.CheckProviderRecoveryAdmission(recovery.recovery_request,
            recovery.recovery_request_hash, canonical_netgroup,
            false, recovery.provider_maximum_network_fee, now, netgroup, error);
    };
    BOOST_CHECK(!admission());
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_HISTORY_FULL");
    BOOST_CHECK(!store.CommitProviderAlternativeRecoveryQuote(recovery,
        environment.netgroup_bucket, Params().GenesisBlock().GetHash(), now, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_HISTORY_FULL");
    BOOST_CHECK(GetMockableDatabase(m_wallet).m_records == full);

    BOOST_REQUIRE(batch.ErasePaymasterSession(session.request_id));
    BOOST_REQUIRE_MESSAGE(admission(), error);
    BOOST_REQUIRE_MESSAGE(store.CommitProviderAlternativeRecoveryQuote(recovery,
        environment.netgroup_bucket, Params().GenesisBlock().GetHash(), now, error), error);
    const auto mixed_full = GetMockableDatabase(m_wallet).m_records;
    // Exact recovery replay works at the combined limit and grants no new budget.
    BOOST_REQUIRE_MESSAGE(store.CommitProviderAlternativeRecoveryQuote(recovery,
        environment.netgroup_bucket, Params().GenesisBlock().GetHash(), now, error), error);
    BOOST_CHECK(GetMockableDatabase(m_wallet).m_records == mixed_full);
    BOOST_CHECK(!paymaster_store::internal::CheckProviderHistoryCapacity(batch, session.request_id, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_HISTORY_FULL");
    BOOST_CHECK(!paymaster_store::internal::CheckProviderHistoryCapacity(batch, recovery.request_id,
        error, uint256S("1853")));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_HISTORY_FULL");
}

BOOST_AUTO_TEST_CASE(expired_unsigned_provider_recovery_history_is_compacted_atomically)
{
    constexpr int64_t now{1750};
    auto fixture = MakeProviderRecoveryQuoteFixture(now,
        "550e8400-e29b-41d4-a716-446655441119", uint256S("1861"));
    const auto& recovery = fixture.recovery;
    const auto genesis = Params().GenesisBlock().GetHash();
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(m_wallet, fixture.environment, {fixture.pool_entry}));
    PaymasterStore store{m_wallet};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.CommitProviderAlternativeRecoveryQuote(recovery,
        fixture.environment.netgroup_bucket, genesis, now, error), error);
    auto& database = GetMockableDatabase(m_wallet);
    const auto quoted = database.m_records;
    size_t expired{0};
    const int64_t expiry = recovery.capacity_request.expires_at + 1;
    // The first expiry pass releases pool/budget obligations. Independent
    // unreadable authority must veto that release, not just later pruning.
    for (const auto* record_type : {&DBKeys::PAYMASTER_PROVIDER_COMMIT,
                                   &DBKeys::PAYMASTER_RESULT,
                                   &DBKeys::PAYMASTER_USER_AUTH}) {
        for (const bool future_version : {false, true}) {
            auto raw = database.MakeBatch();
            const auto key = std::make_pair(*record_type, recovery.recovery_response.recovery_commit_key);
            BOOST_REQUIRE(future_version ? raw->Write(key, uint16_t{65535}) : raw->Write(key, uint8_t{0}));
            const auto unknown = database.m_records;
            BOOST_CHECK(!store.ExpireAlternativeRecoveries(expiry, expired, error));
            BOOST_CHECK_EQUAL(expired, 0U);
            BOOST_CHECK(database.m_records == unknown);
            database.m_records = quoted;
        }
    }
    for (int failure = 0; failure < 3; ++failure) {
        if (failure == 0) database.m_txn_pass = false;
        if (failure == 1) database.FailWriteAt(0);
        if (failure == 2) database.FailCommit();
        BOOST_CHECK(!store.ExpireAlternativeRecoveries(expiry, expired, error));
        BOOST_CHECK_EQUAL(expired, 0U);
        database.m_txn_pass = true;
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == quoted);
        if (failure == 2) {
            BOOST_CHECK(!CheckPaymasterCheckpoint(database, error));
            AlignSecurityFixtureCheckpoint(database);
        }
    }
    BOOST_REQUIRE_MESSAGE(store.ExpireAlternativeRecoveries(expiry, expired, error), error);
    BOOST_CHECK_EQUAL(expired, 1U);
    AlternativeRecoveryRecord saved;
    BOOST_REQUIRE(store.GetAlternativeRecoveryById(recovery.recovery_id, saved));
    BOOST_REQUIRE(saved.expired);
    ProviderBudgetLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(ReadProviderSecurityState(m_wallet, ledger, pool));
    BOOST_REQUIRE_EQUAL(pool.size(), 1U);
    BOOST_CHECK(pool.front().state == PoolEntryState::AVAILABLE);
    const auto released = std::find_if(ledger.reservations.begin(), ledger.reservations.end(),
        [&](const ProviderBudgetReservation& entry) { return entry.commit_key == recovery.recovery_response.recovery_commit_key; });
    BOOST_REQUIRE(released != ledger.reservations.end());
    BOOST_CHECK(released->state == BudgetReservationState::RELEASED);
    const int64_t boundary = expiry + paymaster_store::internal::CAPACITY_REPLAY_RETENTION_SECONDS;
    const auto retained = database.m_records;
    BOOST_REQUIRE(store.ExpireAlternativeRecoveries(boundary, expired, error));
    BOOST_CHECK(database.m_records == retained);
    for (int failure = 0; failure < 3; ++failure) {
        if (failure == 0) database.m_txn_pass = false;
        if (failure == 1) database.FailWriteAt(0);
        if (failure == 2) database.FailCommit();
        BOOST_CHECK(!store.ExpireAlternativeRecoveries(boundary + 1, expired, error));
        database.m_txn_pass = true;
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == retained);
        if (failure == 2) {
            BOOST_CHECK(!CheckPaymasterCheckpoint(database, error));
            AlignSecurityFixtureCheckpoint(database);
        }
    }

    // Neither an unsafe local label nor independent retained authority may be
    // erased. Restore raw fixture rows only between mutually exclusive cases.
    const auto recovery_key = std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, recovery.recovery_id);
    auto unsafe = saved;
    unsafe.user_signed_psbt = {1};
    BOOST_REQUIRE(database.MakeBatch()->Write(recovery_key, unsafe));
    auto before = database.m_records;
    BOOST_CHECK(!store.ExpireAlternativeRecoveries(boundary + 1, expired, error));
    BOOST_CHECK(database.m_records == before);
    database.m_records = retained;
    auto protected_ledger = ledger;
    protected_ledger.reservations.front().state = BudgetReservationState::RESERVED;
    BOOST_REQUIRE(database.MakeBatch()->Write(DBKeys::PAYMASTER_PROVIDER_BUDGET, protected_ledger));
    before = database.m_records;
    BOOST_CHECK(!store.ExpireAlternativeRecoveries(boundary + 1, expired, error));
    BOOST_CHECK(database.m_records == before);
    database.m_records = retained;
    // Even unreadable commit evidence is protective, rather than absence.
    BOOST_REQUIRE(database.MakeBatch()->Write(std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT,
        recovery.recovery_response.recovery_commit_key), uint8_t{0}));
    before = database.m_records;
    BOOST_CHECK(!store.ExpireAlternativeRecoveries(boundary + 1, expired, error));
    BOOST_CHECK(database.m_records == before);
    database.m_records = retained;

    wallet::CWallet restarted{m_node.chain.get(), "recovery-history-restart",
        DuplicateSecurityFixtureDatabase(database)};
    PaymasterStore restarted_store{restarted};
    BOOST_REQUIRE_MESSAGE(restarted_store.ExpireAlternativeRecoveries(boundary + 1, expired, error), error);
    BOOST_CHECK(restarted_store.GetAlternativeRecoveryByIdWithStatus(recovery.recovery_id, saved) == DatabaseReadStatus::NOT_FOUND);
    BOOST_CHECK(!restarted_store.CommitProviderAlternativeRecoveryQuote(recovery,
        fixture.environment.netgroup_bucket, genesis, boundary + 2, error));
    // The global replay rows also expire after their authenticated signing
    // horizon, although the alternative owner was already pruned on restart.
    ProviderBudgetLedger cache_before;
    std::vector<ProviderPoolEntry> pool_before;
    BOOST_REQUIRE(ReadProviderSecurityState(restarted, cache_before, pool_before));
    const auto& cache = fixture.environment.capacity_records.front();
    const int64_t cache_expiry = recovery.capacity_request.expires_at + DEFAULT_RETRY_SECONDS + MAX_QUOTE_TTL_SECONDS + 1;
    BOOST_REQUIRE_MESSAGE(restarted_store.ExpireProviderCapacityReservations(cache_expiry, expired, error), error);
    BOOST_CHECK_EQUAL(expired, 0U);
    WalletBatch cache_batch{restarted.GetDatabase()};
    std::vector<unsigned char> proof;
    uint256 indexed;
    BOOST_CHECK(cache_batch.ReadPaymasterCapacityResponseWithStatus(cache.request_hash, proof) == DatabaseReadStatus::NOT_FOUND);
    BOOST_CHECK(cache_batch.ReadPaymasterCapacityNonceWithStatus(cache.client_nonce, indexed) == DatabaseReadStatus::NOT_FOUND);
    BOOST_CHECK(cache_batch.ReadPaymasterCapacitySessionWithStatus(cache.session_key, indexed) == DatabaseReadStatus::NOT_FOUND);
    ProviderBudgetLedger cache_after;
    std::vector<ProviderPoolEntry> pool_after;
    BOOST_REQUIRE(ReadProviderSecurityState(restarted, cache_after, pool_after));
    cache_before.capacity_admissions.clear();
    BOOST_CHECK(SerializePaymasterSecurityObject(cache_before) == SerializePaymasterSecurityObject(cache_after));
    BOOST_CHECK(SerializePaymasterSecurityObject(pool_before) == SerializePaymasterSecurityObject(pool_after));
    BOOST_REQUIRE(CheckPaymasterCheckpoint(restarted.GetDatabase(), error));
}

BOOST_AUTO_TEST_CASE(provider_recovery_pruning_is_bounded_and_preserves_signed_records)
{
    constexpr int64_t now{1750};
    auto fixture = MakeProviderRecoveryQuoteFixture(now,
        "550e8400-e29b-41d4-a716-446655441120", uint256S("1871"));
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(m_wallet, fixture.environment, {fixture.pool_entry}));
    PaymasterStore store{m_wallet};
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.CommitProviderAlternativeRecoveryQuote(fixture.recovery,
        fixture.environment.netgroup_bucket, Params().GenesisBlock().GetHash(), now, error), error);
    auto signed_recovery = fixture.recovery;
    signed_recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
    signed_recovery.user_signed_psbt = {1};
    ++signed_recovery.updated_at;
    BOOST_REQUIRE_MESSAGE(store.UpdateAlternativeRecovery(signed_recovery, error), error);
    const int64_t observation = now + 100 + paymaster_store::internal::CAPACITY_REPLAY_RETENTION_SECONDS;
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.TxnBegin());
        for (size_t i = 0; i < paymaster_store::internal::MAX_PROVIDER_SESSION_PRUNES_PER_PASS + 2; ++i) {
            const auto id = strprintf("550e8400-e29b-41d4-a716-%012u", i);
            auto old = MakeProviderRecoveryQuoteFixture(now, id.c_str(), SecurityTestId(uint256S("1872"), i + 1), &fixture.environment);
            old.recovery.expired = true;
            old.recovery.updated_at = now + 40;
            BOOST_REQUIRE_MESSAGE(paymaster_store::internal::ValidateAlternativeRecoveryRecordShape(old.recovery, error), error);
            BOOST_REQUIRE(batch.WritePaymasterAlternativeRecovery(old.recovery));
        }
        BOOST_REQUIRE(batch.TxnCommit());
    }
    size_t expired{0};
    auto& source_database = GetMockableDatabase(m_wallet);
    const auto corrupt_owner = source_database.m_records;
    BOOST_CHECK(!store.CloseExpiredProviderRecovery(signed_recovery.recovery_id, observation, error));
    BOOST_CHECK(source_database.m_records == corrupt_owner);
    // The deliberately corrupt USER evidence must veto its own release. Test
    // bounded pruning independently on an image without that corrupt owner;
    // the original database retains its proof and all financial bindings.
    auto compact_database = DuplicateSecurityFixtureDatabase(source_database);
    BOOST_REQUIRE(compact_database->MakeBatch()->Erase(
        std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, signed_recovery.recovery_id)));
    wallet::CWallet compact_wallet{m_node.chain.get(), "bounded-recovery-pruning", std::move(compact_database)};
    AlignSecurityFixtureCheckpoint(compact_wallet.GetDatabase());
    PaymasterStore compact_store{compact_wallet};
    std::vector<AlternativeRecoveryRecord> first_page;
    uint256 next;
    BOOST_REQUIRE(WalletBatch{compact_wallet.GetDatabase()}.ListPaymasterAlternativeRecoveriesPage({}, first_page, next));
    BOOST_REQUIRE_EQUAL(first_page.size(), 64U);
    BOOST_CHECK(!next.IsNull());
    const size_t prunable = std::count_if(first_page.begin(), first_page.end(), [](const auto& record) {
        return record.expired && record.phase <= AlternativeRecoveryPhase::RESPONSE_VALIDATED;
    });
    BOOST_REQUIRE_MESSAGE(compact_store.ExpireAlternativeRecoveries(observation, expired, error), error);
    std::vector<AlternativeRecoveryRecord> remaining;
    BOOST_REQUIRE(WalletBatch{compact_wallet.GetDatabase()}.ListPaymasterAlternativeRecoveries(remaining));
    BOOST_CHECK_EQUAL(remaining.size(), 66U - prunable);
    BOOST_REQUIRE_MESSAGE(compact_store.ExpireAlternativeRecoveries(observation, expired, error), error);
    BOOST_REQUIRE(WalletBatch{compact_wallet.GetDatabase()}.ListPaymasterAlternativeRecoveries(remaining));
    BOOST_CHECK(remaining.empty());
    BOOST_CHECK(source_database.m_records == corrupt_owner);
    // The independent image advanced the same provider's external checkpoint;
    // restore this fixture's current checkpoint before testing its own cache.
    AlignSecurityFixtureCheckpoint(m_wallet.GetDatabase());
    // A signed owner keeps its own proof and all economic authority. Only its
    // redundant wire-response replay cache is compacted after the horizon.
    ProviderBudgetLedger ledger_before;
    std::vector<ProviderPoolEntry> pool_before;
    BOOST_REQUIRE(ReadProviderSecurityState(m_wallet, ledger_before, pool_before));
    const int64_t cache_expiry = signed_recovery.capacity_request.expires_at + DEFAULT_RETRY_SECONDS + MAX_QUOTE_TTL_SECONDS + 1;
    BOOST_REQUIRE_MESSAGE(store.ExpireProviderCapacityReservations(cache_expiry, expired, error), error);
    AlternativeRecoveryRecord signed_after;
    BOOST_REQUIRE(store.GetAlternativeRecoveryById(signed_recovery.recovery_id, signed_after));
    BOOST_CHECK(SerializePaymasterSecurityObject(signed_recovery) == SerializePaymasterSecurityObject(signed_after));
    ProviderBudgetLedger ledger_after;
    std::vector<ProviderPoolEntry> pool_after;
    BOOST_REQUIRE(ReadProviderSecurityState(m_wallet, ledger_after, pool_after));
    ledger_before.capacity_admissions.clear();
    BOOST_CHECK(SerializePaymasterSecurityObject(ledger_before) == SerializePaymasterSecurityObject(ledger_after));
    BOOST_CHECK(SerializePaymasterSecurityObject(pool_before) == SerializePaymasterSecurityObject(pool_after));
}

BOOST_AUTO_TEST_CASE(
    provider_recovery_pre_signature_firewall_reloads_pool_and_budget)
{
    constexpr int64_t now{1750};
    constexpr auto request_id =
        "550e8400-e29b-41d4-a716-446655441018";
    const uint256 seed{uint256S("1751")};
    auto fixture = MakeProviderRecoveryQuoteFixture(now, request_id, seed);
    auto& environment = fixture.environment;
    auto& pool_entry = fixture.pool_entry;
    auto& recovery = fixture.recovery;
    const auto& request = recovery.recovery_request;
    const auto& response = recovery.recovery_response;
    const uint256 genesis_hash{Params().GenesisBlock().GetHash()};
    std::string error;

    BOOST_REQUIRE(PersistProviderQuoteEnvironment(
        m_wallet, environment, {pool_entry}));
    PaymasterStore store{m_wallet};
    BOOST_REQUIRE_MESSAGE(store.CommitProviderAlternativeRecoveryQuote(
                              recovery, environment.netgroup_bucket,
                              genesis_hash, now, error),
                          error);
    recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
    recovery.user_signed_psbt = {0x50, 0x4d, 0x52, 0x45, 0x43};
    recovery.updated_at = now + 1;
    BOOST_REQUIRE_MESSAGE(store.UpdateAlternativeRecovery(recovery, error),
                          error);
    BOOST_REQUIRE(store.GetAlternativeRecoveryById(
        recovery.recovery_id, recovery));

    auto& mock = GetMockableDatabase(m_wallet);
    const MockableData before_validation = mock.m_records;
    BOOST_REQUIRE_MESSAGE(
        store.ValidateProviderAlternativeRecoveryPreSignatureAuthorization(
            recovery, genesis_hash, now + 2, error),
        error);
    BOOST_CHECK(mock.m_records == before_validation);
    for (const int64_t expired_at : {response.expires_at, recovery.capacity_request.expires_at}) {
        BOOST_CHECK(!store.ValidateProviderAlternativeRecoveryPreSignatureAuthorization(
            recovery, genesis_hash, expired_at, error));
        BOOST_CHECK(mock.m_records == before_validation);
    }

    auto pool_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    wallet::CWallet pool_wallet{m_node.chain.get(),
                                "recovery-pre-signature-pool-conflict",
                                std::move(pool_database)};
    {
        LOCK(pool_wallet.cs_wallet);
        WalletBatch batch{pool_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(pool_wallet.GetDatabase());
        std::vector<ProviderPoolEntry> pool;
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(pool));
        BOOST_REQUIRE_EQUAL(pool.size(), 1U);
        pool.front().reservation_id = SecurityTestId(seed, 12);
        BOOST_REQUIRE(pool_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_PROVIDER_POOL, pool));
    }
    PaymasterStore pool_store{pool_wallet};
    BOOST_CHECK(!pool_store
                     .ValidateProviderAlternativeRecoveryPreSignatureAuthorization(
                         recovery, genesis_hash, now + 2, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_RESERVATION_CONFLICT");

    auto budget_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    wallet::CWallet budget_wallet{m_node.chain.get(),
                                  "recovery-pre-signature-budget-conflict",
                                  std::move(budget_database)};
    {
        LOCK(budget_wallet.cs_wallet);
        WalletBatch batch{budget_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(budget_wallet.GetDatabase());
        ProviderBudgetLedger ledger;
        BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(ledger));
        const auto reservation = std::find_if(
            ledger.reservations.begin(), ledger.reservations.end(),
            [&](const ProviderBudgetReservation& entry) {
                return entry.commit_key == response.recovery_commit_key;
            });
        BOOST_REQUIRE(reservation != ledger.reservations.end());
        ++reservation->network_fee.value;
        BOOST_REQUIRE(budget_wallet.GetDatabase().MakeBatch()->Write(DBKeys::PAYMASTER_PROVIDER_BUDGET, ledger));
    }
    PaymasterStore budget_store{budget_wallet};
    BOOST_CHECK(!budget_store
                     .ValidateProviderAlternativeRecoveryPreSignatureAuthorization(
                         recovery, genesis_hash, now + 2, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH");

    // This fixture has authenticated local records but intentionally no live
    // recovery prevouts/PSBT. It tests inbox classification and exact stored
    // result replay, never successful fresh provider signing.
    SyncWithValidationInterfaceQueue();
    wallet::CWallet processing_wallet{m_node.chain.get(), "recovery-submit-classification",
                                      DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase())};
    WalletContext context;
    context.chain = m_node.chain.get();
    context.args = m_node.args;
    std::unique_ptr<Manager> manager;
    {
        LOCK(processing_wallet.cs_wallet);
        processing_wallet.SetLastBlockProcessed(*m_node.chain->getHeight(),
            m_node.chain->getBlockHash(*m_node.chain->getHeight()));
        ProviderSettings settings{environment.settings};
        settings.operation_mode = ProviderOperationMode::MANUAL;
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(
            DBKeys::PAYMASTER_SETTINGS, settings));
    }
    PaymasterStore processing_store{processing_wallet};
    auto& processing_database = GetMockableDatabase(processing_wallet);
    const auto recovery_key = std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, recovery.recovery_id);
    AlternativeRecoveryRecord loaded;
    BOOST_CHECK(processing_store.GetAlternativeRecoveryByIdWithStatus(recovery.recovery_id, loaded) ==
                DatabaseReadStatus::FOUND);
    BOOST_CHECK(processing_store.GetAlternativeRecoveryById(recovery.recovery_id, loaded));
    BOOST_CHECK(processing_store.GetAlternativeRecoveryByIdWithStatus(SecurityTestId(seed, 40), loaded) ==
                DatabaseReadStatus::NOT_FOUND);
    processing_database.m_pass = false;
    BOOST_CHECK(processing_store.GetAlternativeRecoveryByIdWithStatus(recovery.recovery_id, loaded) ==
                DatabaseReadStatus::READ_ERROR);
    BOOST_CHECK(!processing_store.GetAlternativeRecoveryById(recovery.recovery_id, loaded));
    processing_database.m_pass = true;

    AlternativeRecoverySubmit submit;
    submit.genesis_hash = genesis_hash;
    submit.request_id = recovery.request_id;
    submit.session_id = recovery.session_id;
    submit.recovery_id = recovery.recovery_id;
    submit.recovery_provider_id = recovery.recovery_provider_id;
    submit.recovery_request_hash = recovery.recovery_request_hash;
    submit.recovery_commit_key = recovery.recovery_response.recovery_commit_key;
    submit.template_commitment = recovery.recovery_response.manifest.template_commitment;
    submit.user_psbt = recovery.user_signed_psbt;
    BOOST_REQUIRE(ValidateAlternativeRecoverySubmitEnvelope(submit, genesis_hash, error));

    struct RestoreMockTime {
        const std::chrono::seconds original{GetMockTime()};
        ~RestoreMockTime() { SetMockTime(original); }
    } restore_mock_time;
    const auto read_checkpoint = [&] {
        const auto path = PaymasterCheckpointPath(genesis_hash, recovery.recovery_provider_id);
        FILE* file = fsbridge::fopen(path, "rb");
        BOOST_REQUIRE(file);
        std::array<unsigned char, 149> bytes{};
        const bool read = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
        const bool at_end = std::fgetc(file) == EOF;
        const bool closed = std::fclose(file) == 0;
        BOOST_REQUIRE(read && at_end && closed);
        return bytes;
    };
    uint16_t message_sequence{50};
    const auto check_message = [&](const AlternativeRecoverySubmit& incoming,
                                   int64_t observation_time,
                                   bool local_failure,
                                   const std::string& reason,
                                   bool exact_final = false,
                                   bool retain_message = false) {
        manager = std::make_unique<Manager>(true);
        context.paymaster = manager.get();
        BOOST_REQUIRE(manager->StartProvider(processing_wallet.GetName(), recovery.recovery_provider_id));
        SetMockTime(observation_time);
        const uint256 message_id{SecurityTestId(seed, message_sequence++)};
        BOOST_REQUIRE(manager->EnqueueDirectMessage(42, message_id, 100,
                                                  DirectPayload{incoming}, observation_time));
        const MockableData before_processing{processing_database.m_records};
        const auto checkpoint_before = read_checkpoint();
        try {
            const auto result = paymaster_app::ProcessPaymasterSubmits(
                context, processing_wallet, paymaster_app::ExecutionMode::MANUAL);
            BOOST_CHECK(!local_failure);
            BOOST_CHECK(result.HasMessage());
            if (exact_final) {
                BOOST_CHECK(result.queued.value_or(false));
                BOOST_CHECK(!result.IsRejected());
                BOOST_CHECK(result.result_status == PaymasterResultStatus::FINAL_COMMITTED);
            } else {
                BOOST_CHECK(result.IsRejected());
                BOOST_CHECK(!result.queued.value_or(false));
                BOOST_CHECK_EQUAL(result.rejection_reason.value_or(""), reason);
            }
        } catch (const ApplicationError& failure) {
            BOOST_CHECK_MESSAGE(local_failure, failure.what());
            BOOST_CHECK(failure.kind == ErrorKind::WALLET);
            if (!reason.empty()) BOOST_CHECK_EQUAL(failure.what(), reason);
        }
        BOOST_CHECK_EQUAL(manager->DirectMessageCount(), local_failure ? 1U : 0U);
        BOOST_CHECK(manager->IsProviderRunning(processing_wallet.GetName(), recovery.recovery_provider_id));
        BOOST_CHECK(processing_database.m_records == before_processing);
        BOOST_CHECK(read_checkpoint() == checkpoint_before);
        if (!retain_message) {
            BOOST_REQUIRE(manager->AcknowledgeDirectMessagesIfPresent({message_id}));
        }
    };
    const auto write_recovery = [&](const AlternativeRecoveryRecord& candidate) {
        BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(recovery_key, candidate));
    };

    AlternativeRecoveryRecord unsigned_recovery{recovery};
    unsigned_recovery.phase = AlternativeRecoveryPhase::RESPONSE_VALIDATED;
    unsigned_recovery.user_signed_psbt.clear();
    BOOST_REQUIRE_MESSAGE(paymaster_store::internal::ValidateAlternativeRecoveryRecordShape(
                              unsigned_recovery, error), error);
    write_recovery(unsigned_recovery);
    AlternativeRecoverySubmit foreign{submit};
    foreign.template_commitment = SecurityTestId(seed, 41);
    check_message(foreign, now + 1, false, "PAYMASTER_RECOVERY_SUBMIT_BINDING_MISMATCH");
    check_message(submit, response.expires_at, false, "PAYMASTER_RECOVERY_SUBMIT_EXPIRED");
    // Failure to reconstruct local signing authority must not consume even a
    // malformed remote PSBT. No fresh signature is possible from this fixture.
    check_message(submit, now + 1, true, "");

    AlternativeRecoveryRecord expired{unsigned_recovery};
    expired.expired = true;
    write_recovery(expired);
    check_message(submit, now + 1, false, "PAYMASTER_RECOVERY_SUBMIT_BINDING_MISMATCH");

    AlternativeRecoveryRecord corrupt{unsigned_recovery};
    corrupt.recovery_response.identity_signature.front() ^= 1;
    write_recovery(corrupt);
    check_message(foreign, now + 1, true, "PAYMASTER_RECOVERY_RESPONSE_MISMATCH");

    AlternativeRecoveryRecord future{unsigned_recovery};
    ++future.version;
    write_recovery(future);
    BOOST_CHECK(processing_store.GetAlternativeRecoveryByIdWithStatus(recovery.recovery_id, loaded) ==
                DatabaseReadStatus::UNSUPPORTED_VERSION);
    BOOST_CHECK(!processing_store.GetAlternativeRecoveryById(recovery.recovery_id, loaded));
    check_message(submit, now + 1, true, "PAYMASTER_RECOVERY_SUBMIT_DATABASE_READ");
    BOOST_REQUIRE(processing_wallet.GetDatabase().MakeBatch()->Write(
        recovery_key, AlternativeRecoveryRecord::CURRENT_VERSION));
    BOOST_CHECK(processing_store.GetAlternativeRecoveryByIdWithStatus(recovery.recovery_id, loaded) ==
                DatabaseReadStatus::READ_ERROR);
    check_message(submit, now + 1, true, "PAYMASTER_RECOVERY_SUBMIT_DATABASE_READ");

    write_recovery(recovery);
    AlternativeRecoverySubmit changed_psbt{submit};
    changed_psbt.user_psbt.push_back(1);
    check_message(changed_psbt, now + 1, false, "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT");
    check_message(submit, response.expires_at + 1, true, "");

    AlternativeRecoveryRecord final_recovery{recovery};
    final_recovery.phase = AlternativeRecoveryPhase::FINAL_COMMITTED;
    CMutableTransaction final_transaction;
    final_transaction.vin.emplace_back(request.user_dd_inputs.front());
    final_transaction.vout.emplace_back(1, request.wallet_returns.front().script_pub_key);
    final_recovery.final_transaction = SerializePaymasterSecurityObject(final_transaction);
    final_recovery.expected_wtxid = CTransaction{final_transaction}.GetWitnessHash();
    PaymasterResult& final_result = final_recovery.signed_result;
    final_result.genesis_hash = genesis_hash;
    final_result.provider_id = recovery.recovery_provider_id;
    final_result.commit_key = response.recovery_commit_key;
    final_result.result_sequence = 1;
    final_result.status = PaymasterResultStatus::FINAL_COMMITTED;
    final_result.txid = CTransaction{final_transaction}.GetHash();
    final_result.raw_transaction_hash = final_recovery.expected_wtxid;
    final_result.final_transaction = final_transaction;
    final_result.updated_at = now + 2;
    const auto sign_final_result = [&] {
        final_result.identity_signature.resize(64);
        BOOST_REQUIRE(environment.identity_key.SignSchnorr(
            GetPaymasterResultSignatureHash(final_result), final_result.identity_signature,
            nullptr, uint256{}));
    };
    sign_final_result();
    BOOST_REQUIRE_MESSAGE(paymaster_store::internal::ValidateAlternativeRecoveryRecordShape(
                              final_recovery, error), error);
    const AlternativeRecoveryRecord intact_final{final_recovery};
    write_recovery(final_recovery);
    check_message(changed_psbt, now + 3, false, "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT");
    check_message(submit, response.expires_at + 1, false, "", /*exact_final=*/true);

    // A validly signed but wrong local result status must remain a local
    // failure even when the incoming PSBT also conflicts.
    final_result.status = PaymasterResultStatus::BROADCAST_ATTEMPTED;
    sign_final_result();
    write_recovery(final_recovery);
    check_message(changed_psbt, now + 3, true, "PAYMASTER_ALTERNATIVE_RECOVERY_COMMIT_CONFLICT");
    final_result.txid.reset();
    write_recovery(final_recovery);
    check_message(submit, now + 3, true, "PAYMASTER_INVALID_RESULT_ARTIFACTS",
                  /*exact_final=*/false, /*retain_message=*/true);

    // Repair only the corrupt local row. The exact retained message resumes
    // without re-enqueue, cancellation, a fresh signature or another payment.
    write_recovery(intact_final);
    const auto before_retry = processing_database.m_records;
    const auto checkpoint_before_retry = read_checkpoint();
    const auto outbound_before_retry = manager->OutboundDirectMessageCount();
    const auto resumed = paymaster_app::ProcessPaymasterSubmits(
        context, processing_wallet, paymaster_app::ExecutionMode::MANUAL);
    BOOST_CHECK(resumed.HasMessage());
    BOOST_CHECK(resumed.queued.value_or(false));
    BOOST_CHECK(!resumed.IsRejected());
    BOOST_CHECK(resumed.txid == intact_final.signed_result.txid);
    BOOST_CHECK(resumed.result_status == PaymasterResultStatus::FINAL_COMMITTED);
    BOOST_CHECK_EQUAL(manager->DirectMessageCount(), 0U);
    BOOST_CHECK_EQUAL(manager->OutboundDirectMessageCount(), outbound_before_retry + 1);
    BOOST_CHECK(processing_database.m_records == before_retry);
    BOOST_CHECK(read_checkpoint() == checkpoint_before_retry);
}

BOOST_AUTO_TEST_CASE(
    provider_alternative_recovery_budget_binding_is_versioned_and_fail_closed)
{
    constexpr int64_t now{1800};
    ProviderSafetyPolicy safety;
    safety.user_paid = SecurityLimits(/*maximum_reserved=*/2000);
    safety.maximum_active_quotes_total = 8;
    safety.maximum_active_quotes_per_netgroup = 8;
    safety.maximum_active_quotes_per_recipient = 8;
    safety.maximum_quote_requests_per_netgroup_per_minute = 16;
    safety.updated_at = now - 1;

    ProviderBudgetLedger ledger;
    ledger.recipient_bucket_secret = SecurityTestId(uint256S("1800"), 1);
    ledger.accounting_time_high_water = now;
    const uint256 netgroup_bucket =
        SecurityTestId(uint256S("1800"), 2);

    AlternativeRecoveryRecord recovery;
    recovery.provider_side = true;
    recovery.phase = AlternativeRecoveryPhase::RESPONSE_VALIDATED;
    recovery.recovery_response.recovery_provider_id =
        SecurityTestId(uint256S("1800"), 3);
    recovery.recovery_response.recovery_request_hash =
        SecurityTestId(uint256S("1800"), 4);
    recovery.recovery_response.manifest.manifest_id =
        SecurityTestId(uint256S("1800"), 5);
    recovery.recovery_response.manifest.template_commitment =
        SecurityTestId(uint256S("1800"), 6);
    recovery.recovery_response.manifest.network_fee = DGBSatoshis{500};
    recovery.recovery_response.manifest.wallet_returns = {
        {NewTaprootScript(), DDCents{100}}};
    recovery.recovery_response.recovery_commit_key =
        GetAlternativeRecoveryCommitKey(recovery.recovery_response);
    recovery.provider_safety_policy_hash =
        GetProviderSafetyPolicyHash(safety);
    recovery.provider_budget_reservation_id =
        recovery.recovery_response.recovery_commit_key;
    recovery.provider_netgroup_bucket = netgroup_bucket;
    recovery.provider_maximum_network_fee =
        recovery.recovery_response.manifest.network_fee;

    const uint256 recipient_bucket = GetRecipientBudgetBucket(
        ledger,
        recovery.recovery_response.manifest.wallet_returns.front()
            .script_pub_key);
    std::string error;
    BOOST_REQUIRE_MESSAGE(ReserveProviderBudget(
                              ledger, safety, FundingModel::USER_PAID,
                              SponsorshipScope::PUBLIC,
                              recovery.recovery_response.recovery_commit_key,
                              recovery.provider_maximum_network_fee,
                              recipient_bucket, now, error, netgroup_bucket),
                          error);
    BOOST_REQUIRE_MESSAGE(
        ValidateProviderAlternativeRecoveryBudgetAuthorization(
            recovery, &safety, ledger, BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/false, error),
        error);

    ProviderSafetyPolicy changed_safety{safety};
    ++changed_safety.maximum_active_quotes_total;
    changed_safety.updated_at = now + 1;
    BOOST_CHECK(!ValidateProviderAlternativeRecoveryBudgetAuthorization(
        recovery, &changed_safety, ledger,
        BudgetReservationState::RESERVED,
        /*allow_historical_policy=*/false, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH");

    // After USER_SIGNED has become durable, only this exact persisted binding
    // may use the historical reservation.
    recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
    BOOST_REQUIRE_MESSAGE(
        ValidateProviderAlternativeRecoveryBudgetAuthorization(
            recovery, /*policy=*/nullptr, ledger,
            BudgetReservationState::RESERVED,
            /*allow_historical_policy=*/true, error),
        error);
    BOOST_REQUIRE_MESSAGE(SpendProviderBudget(
                              ledger,
                              recovery.recovery_response.recovery_commit_key,
                              now + 2, error),
                          error);
    recovery.phase = AlternativeRecoveryPhase::FINAL_COMMITTED;
    BOOST_REQUIRE_MESSAGE(
        ValidateProviderAlternativeRecoveryBudgetAuthorization(
            recovery, /*policy=*/nullptr, ledger,
            BudgetReservationState::SPENT,
            /*allow_historical_policy=*/true, error),
        error);

    AlternativeRecoveryRecord outdated{recovery};
    --outdated.version;
    BOOST_CHECK(!ValidateProviderAlternativeRecoveryBudgetAuthorization(
        outdated, /*policy=*/nullptr, ledger,
        BudgetReservationState::SPENT,
        /*allow_historical_policy=*/true, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH");

    CDataStream outdated_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    outdated_stream << outdated;
    AlternativeRecoveryRecord decoded_outdated;
    outdated_stream >> decoded_outdated;
    BOOST_CHECK(outdated_stream.empty());
    BOOST_CHECK_EQUAL(decoded_outdated.version,
                      AlternativeRecoveryRecord::CURRENT_VERSION - 1);
    BOOST_CHECK(decoded_outdated.provider_safety_policy_hash ==
                recovery.provider_safety_policy_hash);
    BOOST_CHECK(!ValidateProviderAlternativeRecoveryBudgetAuthorization(
        decoded_outdated, /*policy=*/nullptr, ledger,
        BudgetReservationState::SPENT,
        /*allow_historical_policy=*/true, error));

    AlternativeRecoveryRecord current_serialized{recovery};
    current_serialized.version = AlternativeRecoveryRecord::CURRENT_VERSION;
    current_serialized.capacity_proof_claim_candidate = {1, 2, 3};
    CDataStream current_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    current_stream << current_serialized;
    AlternativeRecoveryRecord decoded_current;
    current_stream >> decoded_current;
    BOOST_CHECK(current_stream.empty());
    BOOST_CHECK_EQUAL(decoded_current.version,
                      AlternativeRecoveryRecord::CURRENT_VERSION);
    BOOST_CHECK(decoded_current.capacity_proof_claim_candidate ==
                current_serialized.capacity_proof_claim_candidate);
}

BOOST_AUTO_TEST_CASE(concurrent_provider_quotes_cannot_overbook_last_budget)
{
    constexpr int64_t now{2000};
    ProviderSecurityEnvironment environment =
        MakeProviderEnvironment(now, /*maximum_reserved=*/500);
    ProviderQuoteFixture first;
    ProviderQuoteFixture second;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(
                              environment,
                              "550e8400-e29b-41d4-a716-446655441002",
                              uint256S("2001"), now, first, error),
                          error);
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(
                              environment,
                              "550e8400-e29b-41d4-a716-446655441003",
                              uint256S("2002"), now, second, error),
                          error);
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(
        m_wallet, environment, {first.pool_entry, second.pool_entry}));

    PaymasterStore store{m_wallet};
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    bool results[2]{};
    std::string errors[2];
    const ProviderAttempt attempts[2]{first.attempt, second.attempt};
    const uint256 expected_genesis[2]{
        first.request.intent.genesis_hash,
        second.request.intent.genesis_hash};
    std::thread workers[2];
    for (size_t index = 0; index < 2; ++index) {
        workers[index] = std::thread{[&, index] {
            ++ready;
            while (!start.load())
                std::this_thread::yield();
            results[index] =
                store.CommitProviderQuote(
                    attempts[index], expected_genesis[index], now,
                    errors[index]);
        }};
    }
    while (ready.load() != 2)
        std::this_thread::yield();
    start = true;
    for (std::thread& worker : workers)
        worker.join();

    BOOST_CHECK_NE(results[0], results[1]);
    const size_t winner = results[0] ? 0 : 1;
    const size_t loser = 1 - winner;
    BOOST_CHECK_EQUAL(errors[loser], "PAYMASTER_SAFETY_LIMIT_EXHAUSTED");

    ProviderBudgetLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(ReadProviderSecurityState(m_wallet, ledger, pool));
    BOOST_REQUIRE_EQUAL(ledger.reservations.size(), 1U);
    BOOST_CHECK_EQUAL(ledger.reservations.front().commit_key,
                      attempts[winner].commit_key);
    BOOST_CHECK(ledger.reservations.front().state ==
                BudgetReservationState::RESERVED);
    BOOST_CHECK(FindBudgetReservation(ledger, attempts[loser].commit_key) ==
                nullptr);

    PaymentSession winner_session;
    PaymentSession loser_session;
    BOOST_CHECK(store.GetSessionByRequestId(
        winner == 0 ? first.request.intent.request_id : second.request.intent.request_id,
        winner_session));
    BOOST_CHECK(!store.GetSessionByRequestId(
        loser == 0 ? first.request.intent.request_id : second.request.intent.request_id,
        loser_session));
    BOOST_REQUIRE_EQUAL(pool.size(), 2U);
    const auto winner_pool = std::find_if(
        pool.begin(), pool.end(), [&](const ProviderPoolEntry& entry) {
            return entry.reservation_id == attempts[winner].commit_key;
        });
    BOOST_REQUIRE(winner_pool != pool.end());
    const ProviderQuoteFixture& loser_fixture = loser == 0 ? first : second;
    const auto loser_pool = std::find_if(
        pool.begin(), pool.end(), [&](const ProviderPoolEntry& entry) {
            return entry.outpoint == loser_fixture.pool_entry.outpoint;
        });
    BOOST_REQUIRE(loser_pool != pool.end());
    BOOST_CHECK_EQUAL(loser_pool->reservation_id,
                      loser_fixture.request.intent.client_nonce);
}

BOOST_AUTO_TEST_CASE(concurrent_provider_quotes_cannot_bind_one_pool_slot_twice)
{
    constexpr int64_t now{2500};
    ProviderSecurityEnvironment environment =
        MakeProviderEnvironment(now, /*maximum_reserved=*/2000);
    CMutableTransaction provider_previous;
    provider_previous.vin.emplace_back(
        COutPoint{SecurityTestId(uint256S("2500"), 1), 0});
    provider_previous.vout.emplace_back(
        2000, environment.identity.identity_script);
    const VerifiedDGBInput shared_provider_input{
        COutPoint{CTransaction{provider_previous}.GetHash(), 0},
        provider_previous, DGBSatoshis{2000}};
    const uint256 shared_client_nonce =
        SecurityTestId(uint256S("2500"), 2);

    ProviderQuoteFixture first;
    ProviderQuoteFixture second;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(
                              environment,
                              "550e8400-e29b-41d4-a716-446655441005",
                              uint256S("2501"), now, first, error,
                              &shared_provider_input, shared_client_nonce),
                          error);
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(
                              environment,
                              "550e8400-e29b-41d4-a716-446655441006",
                              uint256S("2502"), now, second, error,
                              &shared_provider_input, shared_client_nonce),
                          error);
    BOOST_REQUIRE(first.pool_entry.outpoint == second.pool_entry.outpoint);
    // A client nonce has one canonical durable capacity response. Persist the
    // first response and prove that a conflicting second session cannot use
    // the same nonce/output even when its quote races the valid continuation.
    BOOST_REQUIRE_EQUAL(environment.capacity_records.size(), 2U);
    environment.capacity_records.pop_back();
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(
        m_wallet, environment, {first.pool_entry}));

    PaymasterStore store{m_wallet};
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    bool results[2]{};
    std::string errors[2];
    const ProviderAttempt attempts[2]{first.attempt, second.attempt};
    const uint256 expected_genesis[2]{
        first.request.intent.genesis_hash,
        second.request.intent.genesis_hash};
    std::thread workers[2];
    for (size_t index = 0; index < 2; ++index) {
        workers[index] = std::thread{[&, index] {
            ++ready;
            while (!start.load())
                std::this_thread::yield();
            results[index] =
                store.CommitProviderQuote(
                    attempts[index], expected_genesis[index], now,
                    errors[index]);
        }};
    }
    while (ready.load() != 2)
        std::this_thread::yield();
    start = true;
    for (std::thread& worker : workers)
        worker.join();

    BOOST_CHECK(results[0]);
    BOOST_CHECK(!results[1]);
    const size_t winner{0};
    const size_t loser{1};
    BOOST_CHECK_EQUAL(errors[loser],
                      "PAYMASTER_CAPACITY_RESPONSE_BINDING_MISMATCH");

    ProviderBudgetLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(ReadProviderSecurityState(m_wallet, ledger, pool));
    BOOST_REQUIRE_EQUAL(ledger.reservations.size(), 1U);
    BOOST_CHECK_EQUAL(ledger.reservations.front().commit_key,
                      attempts[winner].commit_key);
    BOOST_CHECK(FindBudgetReservation(ledger, attempts[loser].commit_key) ==
                nullptr);
    BOOST_REQUIRE_EQUAL(pool.size(), 1U);
    BOOST_CHECK_EQUAL(pool.front().reservation_id,
                      attempts[winner].commit_key);

    PaymentSession winner_session;
    PaymentSession loser_session;
    BOOST_CHECK(store.GetSessionByRequestId(
        winner == 0 ? first.request.intent.request_id : second.request.intent.request_id,
        winner_session));
    BOOST_CHECK(!store.GetSessionByRequestId(
        loser == 0 ? first.request.intent.request_id : second.request.intent.request_id,
        loser_session));
}

BOOST_AUTO_TEST_CASE(outdated_client_finals_are_not_recoverable)
{
    constexpr int64_t now{2800};
    constexpr CAmount network_fee{100000};
    constexpr CAmount transaction_output_value{100000};
    constexpr auto request_id =
        "550e8400-e29b-41d4-a716-446655441007";
    ProviderSecurityEnvironment environment =
        MakeProviderEnvironment(now, /*maximum_reserved=*/network_fee * 2);
    environment.policy.maximum_network_fee = DGBSatoshis{network_fee};
    environment.settings.policy_hash =
        GetProviderPolicyHash(environment.policy);
    environment.safety.public_sponsored =
        SecurityLimits(/*maximum_reserved=*/network_fee * 2);
    environment.safety.public_sponsored.maximum_network_fee_per_transaction =
        DGBSatoshis{network_fee};
    environment.safety.public_sponsored.maximum_network_fee_per_hour =
        DGBSatoshis{network_fee * 10};
    environment.safety.public_sponsored.maximum_network_fee_per_day =
        DGBSatoshis{network_fee * 100};

    ProviderQuoteFixture fixture;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(
                              environment, request_id, uint256S("2801"), now,
                              fixture, error,
                              /*shared_provider_input=*/nullptr,
                              /*shared_client_nonce=*/{},
                              DGBSatoshis{network_fee},
                              transaction_output_value,
                              Params().GenesisBlock().GetHash()),
                          error);
    PaymentSession session;
    PaymasterResult result;
    CMutableTransaction final_transaction;
    BOOST_REQUIRE_MESSAGE(BuildDurableClientFinalFixture(
                              m_wallet, environment, fixture, now, session,
                              result, final_transaction, error),
                          error);
    {
        LOCK(::cs_main);
        const VerifiedDGBInput& provider_input =
            fixture.response.quote.reserved_dgb_inputs.front();
        const int confirmed_height =
            m_node.chainman->ActiveChain().Height();
        BOOST_REQUIRE_GE(confirmed_height, 0);
        m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(
            provider_input.outpoint,
            Coin{provider_input.creating_tx.vout.at(
                     provider_input.outpoint.n),
                 /*height=*/confirmed_height, /*coinbase=*/false},
            /*possible_overwrite=*/false);
    }
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        UserAuthorizationRecord authorization;
        authorization.commit_key = fixture.attempt.commit_key;
        authorization.attempt_id = fixture.attempt.attempt_id;
        authorization.canonical_psbt_hash =
            Hash(fixture.attempt.user_signed_psbt);
        authorization.accepted_at =
            fixture.attempt.client_manifest_accepted_at;
        authorization.retry_until = fixture.attempt.retry_until;
        BOOST_REQUIRE(batch.WritePaymasterAttempt(fixture.attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
        BOOST_REQUIRE(batch.WritePaymasterResult(result, false));
        BOOST_REQUIRE(batch.WritePaymasterUserAuthorization(authorization));
    }

    const CTransaction final{final_transaction};
    const auto validate_outdated_variant =
        [&](const std::string& wallet_name, auto&& mutate,
            std::string& variant_error) {
            auto database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
            wallet::CWallet outdated_wallet{
                m_node.chain.get(), wallet_name, std::move(database)};
            BOOST_REQUIRE(outdated_wallet.LoadWallet() == DBErrors::LOAD_OK);
            {
                LOCK(outdated_wallet.cs_wallet);
                WalletBatch batch{outdated_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(outdated_wallet.GetDatabase());
                ProviderAttempt persisted;
                BOOST_REQUIRE(batch.ReadPaymasterAttempt(
                    fixture.attempt.attempt_id, persisted));
                mutate(persisted, batch);
                BOOST_REQUIRE(outdated_wallet.GetDatabase().MakeBatch()->Write(std::make_pair(DBKeys::PAYMASTER_ATTEMPT, persisted.attempt_id), persisted));
            }
            PaymasterStore outdated_store{outdated_wallet};
            return outdated_store.ValidateClientDurableFinalForBroadcast(
                final, now + 2,
                /*final_proof=*/nullptr, variant_error);
        };

    const auto make_v2 = [](ProviderAttempt& persisted, WalletBatch&) {
        persisted.client_manifest.version = 2;
        persisted.client_manifest.manifest_id =
            GetClientAuthorizationManifestId(persisted.client_manifest);
        persisted.accepted_client_manifest_id =
            persisted.client_manifest.manifest_id;
    };
    BOOST_CHECK(!validate_outdated_variant(
        "outdated-client-v2", make_v2, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_MANIFEST_INVALID");
    BOOST_CHECK(!validate_outdated_variant(
        "outdated-client-v1",
        [](ProviderAttempt& persisted, WalletBatch&) {
            persisted.client_manifest.version = 1;
            persisted.client_manifest.manifest_id =
                GetClientAuthorizationManifestId(
                    persisted.client_manifest);
            persisted.capacity_snapshot.version =
                ValidatedCapacitySnapshot::CURRENT_VERSION - 1;
            persisted.accepted_client_manifest_id =
                persisted.client_manifest.manifest_id;
        },
        error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_AUTH_MANIFEST_INVALID");
    BOOST_CHECK(!validate_outdated_variant(
        "outdated-client-manifestless",
        [](ProviderAttempt& persisted, WalletBatch&) {
            persisted.client_manifest = {};
            persisted.accepted_client_manifest_id
                .SetNull();
            persisted.client_manifest_accepted_at = 0;
        },
        error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CLIENT_FINAL_AUTHORIZATION_REQUIRED");

    BOOST_CHECK(!validate_outdated_variant(
        "outdated-client-missing-result",
        [&](ProviderAttempt& persisted, WalletBatch& batch) {
            make_v2(persisted, batch);
            BOOST_REQUIRE(batch.ErasePaymasterResult(
                persisted.commit_key));
        },
        error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINAL_RESULT_MISSING");

    BOOST_CHECK(!validate_outdated_variant(
        "outdated-client-mismatched-result",
        [&](ProviderAttempt& persisted, WalletBatch& batch) {
            make_v2(persisted, batch);
            PaymasterResult mismatched{result};
            CMutableTransaction different_transaction{final_transaction};
            ++different_transaction.vout.front().nValue;
            const CTransaction different{different_transaction};
            mismatched.txid = different.GetHash();
            mismatched.raw_transaction_hash = different.GetWitnessHash();
            mismatched.final_transaction = different_transaction;
            mismatched.identity_signature.assign(64, 0);
            BOOST_REQUIRE(environment.identity_key.SignSchnorr(
                GetPaymasterResultSignatureHash(mismatched),
                mismatched.identity_signature, nullptr, uint256{}));
            BOOST_REQUIRE(batch.ErasePaymasterResult(
                persisted.commit_key));
            BOOST_REQUIRE(batch.WritePaymasterResult(
                mismatched, false));
        },
        error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_FINAL_RESULT_BINDING_MISMATCH");

    ProviderAttempt foreign_attempt{fixture.attempt};
    wallet::CWallet foreign_wallet{
        m_node.chain.get(), "current-client-foreign-wallet",
        CreateMockableWalletDatabase()};
    {
        LOCK(foreign_wallet.cs_wallet);
        WalletBatch batch{foreign_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(foreign_wallet.GetDatabase());
        BOOST_REQUIRE(foreign_wallet.GetDatabase().MakeBatch()->Write(std::make_pair(DBKeys::PAYMASTER_ATTEMPT, foreign_attempt.attempt_id), foreign_attempt));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
        BOOST_REQUIRE(batch.WritePaymasterResult(result, false));
        UserAuthorizationRecord authorization;
        authorization.commit_key = foreign_attempt.commit_key;
        authorization.attempt_id = foreign_attempt.attempt_id;
        authorization.canonical_psbt_hash =
            Hash(foreign_attempt.user_signed_psbt);
        authorization.accepted_at =
            foreign_attempt.client_manifest_accepted_at;
        authorization.retry_until = foreign_attempt.retry_until;
        BOOST_REQUIRE(batch.WritePaymasterUserAuthorization(authorization));
    }
    PaymasterStore foreign_store{foreign_wallet};
    BOOST_CHECK(!foreign_store.ValidateClientDurableFinalForBroadcast(
        final, now + 2, /*final_proof=*/nullptr, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_CLIENT_INPUT_NOT_WALLET_OWNED");

    {
        LOCK(::cs_main);
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().CoinsTip().SpendCoin(
            fixture.response.quote.reserved_dgb_inputs.front().outpoint));
    }
}

BOOST_FIXTURE_TEST_CASE(provider_final_commit_spends_budget_atomically,
                        PaymasterMempoolTestingSetup)
{
    constexpr int64_t now{3000};
    constexpr CAmount network_fee{100000};
    constexpr CAmount transaction_output_value{100000};
    constexpr auto request_id = "550e8400-e29b-41d4-a716-446655441004";
    ProviderSecurityEnvironment environment =
        MakeProviderEnvironment(now, /*maximum_reserved=*/network_fee * 2);
    environment.policy.maximum_network_fee = DGBSatoshis{network_fee};
    environment.settings.policy_hash =
        GetProviderPolicyHash(environment.policy);
    environment.safety.public_sponsored =
        SecurityLimits(/*maximum_reserved=*/network_fee * 2);
    environment.safety.public_sponsored.maximum_network_fee_per_transaction =
        DGBSatoshis{network_fee};
    environment.safety.public_sponsored.maximum_network_fee_per_hour =
        DGBSatoshis{network_fee * 10};
    environment.safety.public_sponsored.maximum_network_fee_per_day =
        DGBSatoshis{network_fee * 100};
    ProviderQuoteFixture fixture;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(
                              environment, request_id, uint256S("3001"), now,
                              fixture, error,
                              /*shared_provider_input=*/nullptr,
                              /*shared_client_nonce=*/{},
                              DGBSatoshis{network_fee},
                              transaction_output_value, Params().GenesisBlock().GetHash()),
                          error);
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(
        m_wallet, environment, {fixture.pool_entry}));
    PaymasterStore store{m_wallet};
    BOOST_REQUIRE_MESSAGE(
        store.CommitProviderQuote(
            fixture.attempt, fixture.request.intent.genesis_hash, now,
            error),
        error);
    BOOST_REQUIRE_MESSAGE(
        AddTaprootSigningKey(m_wallet, fixture.user_key, error), error);
    BOOST_REQUIRE_MESSAGE(
        AddTaprootSigningKey(m_wallet, environment.identity_key, error),
        error);
    BOOST_REQUIRE(!fixture.response.quote.reserved_dgb_inputs.empty());
    const CTransactionRef provider_pool_tx = MakeTransactionRef(
        CMutableTransaction{
            fixture.response.quote.reserved_dgb_inputs.front().creating_tx});
    {
        LOCK(m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.AddToWallet(provider_pool_tx,
                                           TxStateInMempool{}));
    }

    PartiallySignedTransaction signed_psbt;
    {
        SpanReader stream{::PROTOCOL_VERSION,
                          fixture.attempt.unsigned_psbt};
        stream >> signed_psbt;
        BOOST_REQUIRE(stream.empty());
    }
    CollaborativePSBTTemplate trusted;
    trusted.psbt = signed_psbt;
    trusted.input_roles = {
        InputRole::USER_DD, InputRole::PROVIDER_DGB};
    BOOST_REQUIRE_MESSAGE(SignCollaborativePSBTForParty(
                              m_wallet, signed_psbt, trusted,
                              SigningParty::USER, error),
                          error);

    ProviderAttempt provider_signed = fixture.attempt;
    provider_signed.state = AttemptState::PROVIDER_SIGNED;
    provider_signed.user_signed_psbt =
        SerializePaymasterSecurityObject(signed_psbt);
    BOOST_REQUIRE_MESSAGE(SignCollaborativePSBTForParty(
                              m_wallet, signed_psbt, trusted,
                              SigningParty::PROVIDER, error),
                          error);
    CMutableTransaction final_transaction;
    BOOST_REQUIRE(FinalizeAndExtractPSBT(
        signed_psbt, final_transaction));
    provider_signed.final_transaction =
        SerializePaymasterSecurityObject(final_transaction);
    provider_signed.final_txid =
        CTransaction{final_transaction}.GetHash();
    provider_signed.provider_signed_at = now + 1;
    provider_signed.updated_at = provider_signed.provider_signed_at;

    PaymentSession session;
    BOOST_REQUIRE(store.GetSessionByRequestId(request_id, session));
    session.state = SessionState::PENDING_PROVIDER;
    session.pending_phase = PendingPhase::PROVIDER_SIGNED_KNOWN;
    session.updated_at = now + 1;
    UserAuthorizationRecord authorization;
    authorization.commit_key = provider_signed.commit_key;
    authorization.attempt_id = provider_signed.attempt_id;
    authorization.canonical_psbt_hash =
        Hash(provider_signed.user_signed_psbt);
    authorization.accepted_at = now + 1;
    authorization.retry_until = provider_signed.retry_until;
    ProviderCommitRecord commit;
    commit.commit_key = provider_signed.commit_key;
    commit.provider_id = provider_signed.provider_id;
    commit.quote_id = provider_signed.quote_id;
    commit.template_commitment = provider_signed.template_commitment;
    commit.final_txid = provider_signed.final_txid;
    commit.final_transaction = provider_signed.final_transaction;
    commit.raw_transaction_hash = Hash(commit.final_transaction);
    commit.provider_inputs = {fixture.transaction.vin[1].prevout};
    commit.committed_at = provider_signed.retry_until + 1;
    commit.retry_until = provider_signed.retry_until;

    PaymasterResult result;
    result.genesis_hash = Params().GenesisBlock().GetHash();
    result.provider_id = commit.provider_id;
    result.commit_key = commit.commit_key;
    result.result_sequence = 1;
    result.status = PaymasterResultStatus::FINAL_COMMITTED;
    result.txid = commit.final_txid;
    result.raw_transaction_hash = commit.raw_transaction_hash;
    result.final_transaction = final_transaction;
    result.updated_at = provider_signed.provider_signed_at;
    result.identity_signature.resize(64);
    BOOST_REQUIRE(environment.identity_key.SignSchnorr(
        GetPaymasterResultSignatureHash(result), result.identity_signature,
        nullptr, uint256{}));
    provider_signed.provider_signed_result =
        SerializePaymasterSecurityObject(result);
    {
        LOCK(m_wallet.cs_wallet);
        WalletBatch batch{m_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAttempt(provider_signed));
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
        BOOST_REQUIRE(batch.WritePaymasterUserAuthorization(authorization));
    }
    // The pre-signed result is recovery authority, not a network-visible
    // result. It must remain private until the atomic provider commit.
    PaymasterResult unpublished_result;
    BOOST_CHECK(!store.GetProviderResult(
        provider_signed.commit_key, unpublished_result));

    // Provider-signature evidence keeps its exact liquidity and budget after
    // the unsigned-provider close deadline, including private signed results
    // that have not yet reached the atomic final commit.
    const auto signed_before_expiry = GetMockableDatabase(m_wallet).m_records;
    size_t unsigned_expired{0};
    BOOST_REQUIRE_MESSAGE(store.ExpireProviderQuotes(provider_signed.retry_until + 1,
        unsigned_expired, error), error);
    BOOST_CHECK_EQUAL(unsigned_expired, 0U);
    ProviderAttempt refused_close;
    BOOST_CHECK(!store.CancelProviderQuote(provider_signed.attempt_id,
        provider_signed.retry_until + 1, refused_close, error));
    BOOST_CHECK(!store.CloseExpiredProviderAttempt(provider_signed.attempt_id,
        provider_signed.retry_until + 1, refused_close, error));
    BOOST_CHECK(GetMockableDatabase(m_wallet).m_records == signed_before_expiry);

    ProviderAttempt user_only{provider_signed};
    user_only.state = AttemptState::USER_PSBT_ACCEPTED;
    user_only.provider_signed_at = 0;
    user_only.provider_signed_result.clear();
    user_only.final_transaction.clear();
    user_only.final_txid.SetNull();
    CMutableTransaction rejected_final;
    BOOST_CHECK(!ValidateProviderCommitForExecution(
        user_only, commit, commit.committed_at, rejected_final, error));
    BOOST_CHECK_EQUAL(error,
                      "PAYMASTER_PROVIDER_COMMIT_AUTHORIZATION_STATE");

    ProviderAttempt outdated_attempt{provider_signed};
    --outdated_attempt.version;
    CDataStream outdated_stream{SER_NETWORK, ::PROTOCOL_VERSION};
    outdated_stream << outdated_attempt;
    ProviderAttempt decoded_outdated;
    outdated_stream >> decoded_outdated;
    BOOST_CHECK(outdated_stream.empty());
    BOOST_CHECK_EQUAL(decoded_outdated.version,
                      ProviderAttempt::CURRENT_VERSION - 1);
    BOOST_CHECK_EQUAL(decoded_outdated.provider_signed_at,
                      provider_signed.provider_signed_at);
    BOOST_CHECK(decoded_outdated.provider_signed_result ==
                provider_signed.provider_signed_result);
    BOOST_CHECK(!ValidateProviderCommitForExecution(
        decoded_outdated, commit, commit.committed_at, rejected_final, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_AUTHORIZATION_VERSION");

    bool has_work{false};
    BOOST_REQUIRE(store.HasProviderDrainWork(
        environment.identity.provider_id, has_work, error));
    BOOST_CHECK(has_work);

    auto& database = GetMockableDatabase(m_wallet);
    const MockableData before_commit = database.m_records;

    auto unreadable_pool_database =
        DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    wallet::CWallet unreadable_pool_wallet{
        m_node.chain.get(), "unreadable-provider-pool",
        std::move(unreadable_pool_database)};
    BOOST_REQUIRE(unreadable_pool_wallet.LoadWallet() == DBErrors::LOAD_OK);
    ProviderPoolEntry outdated_pool_entry{fixture.pool_entry};
    --outdated_pool_entry.version;
    {
        auto raw_batch = unreadable_pool_wallet.GetDatabase().MakeBatch();
        BOOST_REQUIRE(raw_batch->Write(
            DBKeys::PAYMASTER_PROVIDER_POOL,
            std::vector<ProviderPoolEntry>{outdated_pool_entry}));
    }
    auto& unreadable_pool_mock =
        GetMockableDatabase(unreadable_pool_wallet);
    const MockableData before_unreadable_pool =
        unreadable_pool_mock.m_records;
    PaymasterStore unreadable_pool_store{unreadable_pool_wallet};
    BOOST_CHECK(!unreadable_pool_store.CommitProviderFinalTransaction(
        request_id, provider_signed.attempt_id, commit, result,
        result.genesis_hash, error));
    BOOST_CHECK_EQUAL(
        error,
        "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION: record=ProviderPoolEntry found=1 expected=2");
    BOOST_CHECK(unreadable_pool_mock.m_records == before_unreadable_pool);

    for (size_t failing_write = 0; failing_write < 6; ++failing_write) {
        database.FailWriteAt(failing_write);
        BOOST_CHECK(!store.CommitProviderFinalTransaction(
            request_id, provider_signed.attempt_id, commit, result,
            result.genesis_hash, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
        database.ClearFailureInjection();
        BOOST_CHECK(database.m_records == before_commit);

        ProviderBudgetLedger ledger;
        std::vector<ProviderPoolEntry> pool;
        BOOST_REQUIRE(ReadProviderSecurityState(m_wallet, ledger, pool));
        const ProviderBudgetReservation* reservation =
            FindBudgetReservation(ledger, provider_signed.commit_key);
        BOOST_REQUIRE(reservation != nullptr);
        BOOST_CHECK(reservation->state == BudgetReservationState::RESERVED);
        BOOST_REQUIRE_EQUAL(pool.size(), 1U);
        BOOST_CHECK(pool.front().state == PoolEntryState::RESERVED);
    }
    database.FailCommit();
    BOOST_CHECK(!store.CommitProviderFinalTransaction(
        request_id, provider_signed.attempt_id, commit, result,
        result.genesis_hash, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before_commit);

    const auto check_unpromotable = [&](const std::string& wallet_name,
                                        auto mutate) {
        auto tampered_database =
            DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
        wallet::CWallet tampered_wallet{
            m_node.chain.get(), wallet_name,
            std::move(tampered_database)};
        BOOST_REQUIRE(tampered_wallet.LoadWallet() == DBErrors::LOAD_OK);
        ProviderAttempt tampered;
        {
            LOCK(tampered_wallet.cs_wallet);
            WalletBatch batch{tampered_wallet.GetDatabase()};
        AlignSecurityFixtureCheckpoint(tampered_wallet.GetDatabase());
            BOOST_REQUIRE(batch.ReadPaymasterAttempt(
                provider_signed.attempt_id, tampered));
            mutate(tampered);
            BOOST_REQUIRE(tampered_wallet.GetDatabase().MakeBatch()->Write(std::make_pair(DBKeys::PAYMASTER_ATTEMPT, tampered.attempt_id), tampered));
        }
        const DurablePaymasterRecoveryReport rejected =
            RecoverDurablePaymasterCommits(
                tampered_wallet, commit.committed_at);
        PaymasterStore tampered_store{tampered_wallet};
        ProviderCommitRecord unexpected;
        BOOST_CHECK(!tampered_store.GetProviderCommit(
            provider_signed.commit_key, unexpected));
        return rejected;
    };
    DurablePaymasterRecoveryReport rejected = check_unpromotable(
        "tampered-provider-result", [](ProviderAttempt& tampered) {
            BOOST_REQUIRE(!tampered.provider_signed_result.empty());
            tampered.provider_signed_result.back() ^= 1;
        });
    BOOST_CHECK(!rejected.errors.empty());
    rejected = check_unpromotable(
        "tampered-provider-final", [](ProviderAttempt& tampered) {
            BOOST_REQUIRE(!tampered.final_transaction.empty());
            tampered.final_transaction.back() ^= 1;
        });
    BOOST_CHECK(!rejected.errors.empty());
    rejected = check_unpromotable(
        "tampered-provider-signing-time", [](ProviderAttempt& tampered) {
            tampered.provider_signed_at = tampered.retry_until + 1;
        });
    BOOST_CHECK(!rejected.errors.empty());
    DurablePaymasterRecoveryReport user_only_report = check_unpromotable(
        "user-only-provider-attempt", [](ProviderAttempt& tampered) {
            tampered.state = AttemptState::USER_PSBT_ACCEPTED;
            tampered.provider_signed_at = 0;
            tampered.provider_signed_result.clear();
            tampered.final_transaction.clear();
            tampered.final_txid.SetNull();
        });
    BOOST_CHECK_EQUAL(user_only_report.candidates, 0U);

    // Make the exact final transaction valid for every crash-boundary replay.
    // The same chainstate coins can be reused after each mempool removal.
    {
        LOCK(::cs_main);
        auto& coins = m_node.chainman->ActiveChainstate().CoinsTip();
        BOOST_REQUIRE_EQUAL(trusted.psbt.inputs.size(),
                            final_transaction.vin.size());
        for (size_t index = 0; index < final_transaction.vin.size(); ++index) {
            CTxOut prevout;
            BOOST_REQUIRE(trusted.psbt.GetInputUTXO(prevout, index));
            BOOST_REQUIRE(!prevout.IsNull());
            coins.AddCoin(
                final_transaction.vin[index].prevout,
                Coin{CTxOut{prevout}, /*height=*/1, /*coinbase=*/false},
                /*possible_overwrite=*/false);
        }
    }
    const CTransactionRef final_ref =
        MakeTransactionRef(final_transaction);

    enum class FinalizationCrashBoundary {
        AFTER_SIGNATURE,
        AFTER_DATABASE_COMMIT,
        AFTER_WALLET_INSERTION,
        AFTER_BROADCAST,
    };
    struct CrashBoundaryCase {
        FinalizationCrashBoundary boundary;
        const char* name;
    };
    const std::vector<CrashBoundaryCase> crash_boundaries{
        {FinalizationCrashBoundary::AFTER_SIGNATURE, "after-signature"},
        {FinalizationCrashBoundary::AFTER_DATABASE_COMMIT,
         "after-database-commit"},
        {FinalizationCrashBoundary::AFTER_WALLET_INSERTION,
         "after-wallet-insertion"},
        {FinalizationCrashBoundary::AFTER_BROADCAST, "after-broadcast"},
    };
    const MockableData signature_boundary_records = database.m_records;
    for (const CrashBoundaryCase& crash_case : crash_boundaries) {
        BOOST_TEST_CONTEXT("crash boundary=" << crash_case.name)
        {
            std::unique_ptr<WalletDatabase> restart_database;
            {
                auto boundary_database =
                    std::make_unique<MockableDatabase>(
                        signature_boundary_records);
                wallet::CWallet boundary_wallet{
                    m_node.chain.get(), crash_case.name,
                    std::move(boundary_database)};
                BOOST_REQUIRE(boundary_wallet.LoadWallet() ==
                              DBErrors::LOAD_OK);
                AlignSecurityFixtureCheckpoint(boundary_wallet.GetDatabase());
                PaymasterStore boundary_store{boundary_wallet};

                const bool database_committed =
                    crash_case.boundary !=
                    FinalizationCrashBoundary::AFTER_SIGNATURE;
                if (database_committed) {
                    BOOST_REQUIRE_MESSAGE(
                        boundary_store.CommitProviderFinalTransaction(
                            request_id, provider_signed.attempt_id, commit,
                            result, result.genesis_hash, error),
                        error);
                }

                const bool wallet_inserted =
                    crash_case.boundary ==
                        FinalizationCrashBoundary::AFTER_WALLET_INSERTION ||
                    crash_case.boundary ==
                        FinalizationCrashBoundary::AFTER_BROADCAST;
                if (wallet_inserted) {
                    LOCK(boundary_wallet.cs_wallet);
                    BOOST_REQUIRE(boundary_wallet.AddToWallet(
                        final_ref, TxStateInactive{},
                        [](CWalletTx& wallet_tx, bool) {
                            wallet_tx.fTimeReceivedIsTxTime = true;
                            wallet_tx.fFromMe = true;
                            wallet_tx.mapValue["paymaster_durable_commit"] =
                                "1";
                            return true;
                        }));
                }

                const bool broadcast =
                    crash_case.boundary ==
                    FinalizationCrashBoundary::AFTER_BROADCAST;
                if (broadcast) {
                    std::string broadcast_error;
                    BOOST_REQUIRE(
                        node::BroadcastTransaction(
                            m_node, final_ref, broadcast_error,
                            /*max_tx_fee=*/0, /*relay=*/true,
                            /*wait_callback=*/false) ==
                        TransactionError::OK);
                }

                ProviderCommitRecord boundary_commit;
                BOOST_CHECK_EQUAL(
                    boundary_store.GetProviderCommit(
                        provider_signed.commit_key, boundary_commit),
                    database_committed);
                {
                    LOCK(boundary_wallet.cs_wallet);
                    BOOST_CHECK_EQUAL(
                        boundary_wallet.GetWalletTx(
                            provider_signed.final_txid) != nullptr,
                        wallet_inserted);
                }
                const bool mempool_contains_final = WITH_LOCK(
                    ::cs_main,
                    return m_node.mempool->get(
                               provider_signed.final_txid) != nullptr);
                BOOST_CHECK_EQUAL(mempool_contains_final, broadcast);

                // This database copy is the simulated abrupt process stop:
                // only records durable at the selected boundary survive.
                restart_database =
                    DuplicateSecurityFixtureDatabase(boundary_wallet.GetDatabase());
            }

            wallet::CWallet restarted_boundary_wallet{
                m_node.chain.get(),
                std::string{"restart-"} + crash_case.name,
                std::move(restart_database)};
            BOOST_REQUIRE(restarted_boundary_wallet.LoadWallet() ==
                          DBErrors::LOAD_OK);
            PaymasterStore restarted_boundary_store{
                restarted_boundary_wallet};
            const DurablePaymasterRecoveryReport boundary_recovery =
                RecoverDurablePaymasterCommits(
                    restarted_boundary_wallet, commit.committed_at);
            BOOST_CHECK_EQUAL(boundary_recovery.candidates, 1U);
            BOOST_CHECK_EQUAL(boundary_recovery.recovered, 1U);
            BOOST_CHECK_EQUAL(boundary_recovery.already_confirmed, 0U);
            const std::string boundary_error =
                boundary_recovery.errors.empty() ? "unexpected empty recovery diagnostic" : boundary_recovery.errors.front();
            BOOST_CHECK_MESSAGE(
                boundary_recovery.errors.empty(), boundary_error);

            ProviderCommitRecord recovered_commit;
            BOOST_REQUIRE(restarted_boundary_store.GetProviderCommit(
                provider_signed.commit_key, recovered_commit));
            BOOST_CHECK(SerializePaymasterSecurityObject(recovered_commit) ==
                        SerializePaymasterSecurityObject(commit));
            PaymasterResult recovered_result;
            BOOST_REQUIRE(restarted_boundary_store.GetProviderResult(
                provider_signed.commit_key, recovered_result));
            BOOST_CHECK(SerializePaymasterSecurityObject(recovered_result) ==
                        SerializePaymasterSecurityObject(result));
            ProviderAttempt recovered_attempt;
            BOOST_REQUIRE(restarted_boundary_store.GetAttempt(
                provider_signed.attempt_id, recovered_attempt));
            BOOST_CHECK(recovered_attempt.state == AttemptState::MEMPOOL);
            BOOST_CHECK(recovered_attempt.provider_signed_result ==
                        provider_signed.provider_signed_result);

            {
                LOCK(restarted_boundary_wallet.cs_wallet);
                const CWalletTx* recovered_wallet_tx =
                    restarted_boundary_wallet.GetWalletTx(
                        provider_signed.final_txid);
                BOOST_REQUIRE(recovered_wallet_tx != nullptr);
                BOOST_CHECK(
                    SerializePaymasterSecurityObject(
                        *recovered_wallet_tx->tx) ==
                    SerializePaymasterSecurityObject(*final_ref));
                BOOST_CHECK_EQUAL(
                    recovered_wallet_tx
                        ->mapValue.at("paymaster_durable_commit"),
                    "1");
            }

            ProviderBudgetLedger recovered_ledger;
            std::vector<ProviderPoolEntry> recovered_pool;
            BOOST_REQUIRE(ReadProviderSecurityState(
                restarted_boundary_wallet, recovered_ledger,
                recovered_pool));
            BOOST_CHECK_EQUAL(
                std::count_if(
                    recovered_ledger.reservations.begin(),
                    recovered_ledger.reservations.end(),
                    [&](const ProviderBudgetReservation& reservation) {
                        return reservation.commit_key ==
                               provider_signed.commit_key;
                    }),
                1U);
            const ProviderBudgetReservation* recovered_reservation =
                FindBudgetReservation(
                    recovered_ledger, provider_signed.commit_key);
            BOOST_REQUIRE(recovered_reservation != nullptr);
            BOOST_CHECK(recovered_reservation->state ==
                        BudgetReservationState::SPENT);
            BOOST_REQUIRE_EQUAL(recovered_pool.size(), 1U);
            BOOST_CHECK(recovered_pool.front().state ==
                        PoolEntryState::COMMITTED);

            // A second startup pass must be an exact, accounting-neutral
            // replay and must never produce another signature or winner.
            const DurablePaymasterRecoveryReport retry_recovery =
                RecoverDurablePaymasterCommits(
                    restarted_boundary_wallet, commit.committed_at + 1);
            BOOST_CHECK_EQUAL(retry_recovery.candidates, 1U);
            BOOST_CHECK_EQUAL(retry_recovery.recovered, 1U);
            BOOST_CHECK(retry_recovery.errors.empty());
            PaymasterResult retried_result;
            BOOST_REQUIRE(restarted_boundary_store.GetProviderResult(
                provider_signed.commit_key, retried_result));
            BOOST_CHECK(SerializePaymasterSecurityObject(retried_result) ==
                        SerializePaymasterSecurityObject(result));
            BOOST_REQUIRE(ReadProviderSecurityState(
                restarted_boundary_wallet, recovered_ledger,
                recovered_pool));
            BOOST_CHECK_EQUAL(
                std::count_if(
                    recovered_ledger.reservations.begin(),
                    recovered_ledger.reservations.end(),
                    [&](const ProviderBudgetReservation& reservation) {
                        return reservation.commit_key ==
                               provider_signed.commit_key;
                    }),
                1U);

            WITH_LOCK(m_node.mempool->cs,
                      m_node.mempool->removeRecursive(
                          *final_ref, MemPoolRemovalReason::CONFLICT));
        }
    }

    // Place the exact transaction in the local mempool without a provider
    // peer message. Startup recovery must promote the orphan and reconcile
    // these exact bytes while the key-bearing wallet is locked.
    const MempoolAcceptResult accepted = WITH_LOCK(
        ::cs_main,
        return m_node.chainman->ProcessTransaction(
            final_ref, /*test_accept=*/false));
    BOOST_REQUIRE_MESSAGE(
        accepted.m_result_type == MempoolAcceptResult::ResultType::VALID ||
            accepted.m_result_type ==
                MempoolAcceptResult::ResultType::MEMPOOL_ENTRY,
        accepted.m_state.GetRejectReason());
    SecureString restart_passphrase{"provider-signed-restart"};
    BOOST_REQUIRE(m_wallet.EncryptWallet(restart_passphrase));
    BOOST_REQUIRE(m_wallet.IsLocked());
    AlignSecurityFixtureCheckpoint(m_wallet.GetDatabase());
    const DurablePaymasterRecoveryReport recovery =
        RecoverDurablePaymasterCommits(
            m_wallet, commit.committed_at);
    BOOST_CHECK_EQUAL(recovery.candidates, 1U);
    BOOST_CHECK_EQUAL(recovery.recovered, 1U);
    BOOST_CHECK(recovery.errors.empty());
    BOOST_CHECK(m_wallet.IsLocked());
    size_t recovery_notifications{0};
    boost::signals2::scoped_connection recovery_connection =
        m_wallet.NotifyTransactionChanged.connect([&](const uint256&, ChangeType) {
            ++recovery_notifications;
        });
    const auto repeat_recovery = RecoverDurablePaymasterCommits(m_wallet, commit.committed_at);
    BOOST_CHECK(repeat_recovery.errors.empty());
    BOOST_CHECK_EQUAL(repeat_recovery.recovered, 1U);
    BOOST_CHECK_EQUAL(recovery_notifications, 0U);
    ProviderCommitRecord promoted_commit;
    BOOST_REQUIRE(store.GetProviderCommit(
        provider_signed.commit_key, promoted_commit));
    BOOST_CHECK_EQUAL(promoted_commit.committed_at,
                      commit.committed_at);

    ProviderBudgetLedger ledger;
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(ReadProviderSecurityState(m_wallet, ledger, pool));
    const ProviderBudgetReservation* reservation =
        FindBudgetReservation(ledger, provider_signed.commit_key);
    BOOST_REQUIRE(reservation != nullptr);
    BOOST_CHECK(reservation->state == BudgetReservationState::SPENT);
    BOOST_REQUIRE_EQUAL(pool.size(), 1U);
    BOOST_CHECK(pool.front().state == PoolEntryState::COMMITTED);
    ProviderAttempt committed_attempt;
    BOOST_REQUIRE(store.GetAttempt(provider_signed.attempt_id,
                                   committed_attempt));
    // Recovery observes the exact transaction already in the mempool and
    // therefore reconciles the durable commit through to the stronger state.
    BOOST_CHECK(committed_attempt.state == AttemptState::MEMPOOL);
    BOOST_REQUIRE(store.HasProviderDrainWork(
        environment.identity.provider_id, has_work, error));
    BOOST_CHECK(!has_work);

    {
        LOCK(m_wallet.cs_wallet);
        m_wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    }
    ProviderPolicy compatible_advertised_policy{environment.policy};
    compatible_advertised_policy.min_payment.value++;
    BOOST_REQUIRE_MESSAGE(SetPaymasterProviderPolicy(
                              m_wallet, compatible_advertised_policy,
                              commit.committed_at + 1, error),
                          error);

    auto restarted_database = DuplicateSecurityFixtureDatabase(m_wallet.GetDatabase());
    wallet::CWallet restarted_wallet{m_node.chain.get(),
                                     "final-security-restart",
                                     std::move(restarted_database)};
    BOOST_REQUIRE(restarted_wallet.LoadWallet() == DBErrors::LOAD_OK);
    PaymasterStore restarted_store{restarted_wallet};
    ProviderBudgetLedger restarted_ledger;
    std::vector<ProviderPoolEntry> restarted_pool;
    BOOST_REQUIRE(ReadProviderSecurityState(
        restarted_wallet, restarted_ledger, restarted_pool));
    reservation =
        FindBudgetReservation(restarted_ledger, provider_signed.commit_key);
    BOOST_REQUIRE(reservation != nullptr);
    BOOST_CHECK(reservation->state == BudgetReservationState::SPENT);
    BOOST_REQUIRE_EQUAL(restarted_pool.size(), 1U);
    BOOST_CHECK(restarted_pool.front().state == PoolEntryState::COMMITTED);
    ProviderAttempt restarted_attempt;
    BOOST_REQUIRE(restarted_store.GetAttempt(
        provider_signed.attempt_id, restarted_attempt));

    ProviderSafetyPolicy changed_policy{environment.safety};
    changed_policy.maximum_active_quotes_per_recipient--;
    changed_policy.updated_at = commit.committed_at + 1;
    {
        LOCK(restarted_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{restarted_wallet.GetDatabase()}
                          .WritePaymasterProviderSafetyPolicy(changed_policy));
    }
    BOOST_CHECK(!restarted_store.ValidateProviderBudgetAuthorization(
        restarted_attempt, BudgetReservationState::SPENT,
        /*allow_historical_policy=*/false, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_BUDGET_BINDING_MISMATCH");
    BOOST_REQUIRE_MESSAGE(
        restarted_store.ValidateProviderBudgetAuthorization(
            restarted_attempt, BudgetReservationState::SPENT,
            /*allow_historical_policy=*/true, error),
        error);
    auto& restarted_mock = GetMockableDatabase(restarted_wallet);
    const MockableData restarted_before_retry = restarted_mock.m_records;
    restarted_mock.FailWriteAt(0);
    BOOST_CHECK(restarted_store.CommitProviderFinalTransaction(
        request_id, provider_signed.attempt_id, commit, result,
        result.genesis_hash, error));
    restarted_mock.ClearFailureInjection();
    BOOST_CHECK(restarted_mock.m_records == restarted_before_retry);
    BOOST_REQUIRE(restarted_store.HasProviderDrainWork(
        environment.identity.provider_id, has_work, error));
    BOOST_CHECK(!has_work);

    // Rolling expenditure can age out while this exact signed transaction is
    // still unconfirmed. Durable retries use retained authority, never a new
    // budget reservation or signature (including after restart/policy change).
    ProviderBudgetLedger missing{restarted_ledger};
    missing.reservations.clear();
    missing.accounting_time_high_water = commit.committed_at + PROVIDER_BUDGET_WINDOW_SECONDS;
    {
        LOCK(restarted_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{restarted_wallet.GetDatabase()}.WritePaymasterProviderBudgetLedger(missing));
    }
    BOOST_CHECK(!restarted_store.ValidateProviderBudgetAuthorization(restarted_attempt,
        BudgetReservationState::SPENT, /*allow_historical_policy=*/true, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_BUDGET_RESERVATION_MISSING");
    const int64_t later = commit.committed_at + PROVIDER_BUDGET_WINDOW_SECONDS + 1;
    ProviderBudgetLedger aged{restarted_ledger};
    BOOST_REQUIRE_MESSAGE(ReserveProviderBudget(aged, environment.safety,
        FundingModel::SPONSORED, SponsorshipScope::PUBLIC,
        SecurityTestId(uint256S("a001"), 1), DGBSatoshis{network_fee},
        SecurityTestId(uint256S("a001"), 2), later, error, environment.netgroup_bucket), error);
    BOOST_REQUIRE(FindBudgetReservation(aged, commit.commit_key) == nullptr);
    {
        LOCK(restarted_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{restarted_wallet.GetDatabase()}.WritePaymasterProviderBudgetLedger(aged));
    }
    BOOST_CHECK(!restarted_store.ValidateProviderBudgetAuthorization(restarted_attempt,
        BudgetReservationState::RESERVED, /*allow_historical_policy=*/true, error));
    BOOST_REQUIRE_MESSAGE(restarted_store.ValidateProviderBudgetAuthorization(restarted_attempt,
        BudgetReservationState::SPENT, /*allow_historical_policy=*/true, error), error);
    const auto before_aged_retry = restarted_mock.m_records;
    restarted_mock.FailWriteAt(0);
    BOOST_REQUIRE_MESSAGE(restarted_store.CommitProviderFinalTransaction(request_id,
        provider_signed.attempt_id, commit, result, result.genesis_hash, error), error);
    restarted_mock.ClearFailureInjection();
    BOOST_CHECK(restarted_mock.m_records == before_aged_retry);
    const auto recovered_after_aging = RecoverDurablePaymasterCommit(restarted_wallet, commit, later);
    BOOST_CHECK_MESSAGE(recovered_after_aging.error.empty(), recovered_after_aging.error);
    ProviderBudgetLedger after_aging;
    BOOST_REQUIRE(GetPaymasterProviderBudgetLedger(restarted_wallet, after_aging));
    BOOST_CHECK(SerializePaymasterSecurityObject(after_aging) == SerializePaymasterSecurityObject(aged));

    // Present conflicting rows and damaged durable artifacts must not qualify.
    auto conflict = restarted_ledger.reservations.front();
    conflict.state = BudgetReservationState::RELEASED;
    auto conflicting_ledger = aged;
    conflicting_ledger.reservations.push_back(conflict);
    {
        LOCK(restarted_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{restarted_wallet.GetDatabase()}.WritePaymasterProviderBudgetLedger(conflicting_ledger));
    }
    BOOST_CHECK(!restarted_store.ValidateProviderBudgetAuthorization(restarted_attempt,
        BudgetReservationState::SPENT, /*allow_historical_policy=*/true, error));
    ProviderCommitRecord damaged{commit};
    damaged.raw_transaction_hash = uint256S("bad");
    {
        LOCK(restarted_wallet.cs_wallet);
        WalletBatch batch{restarted_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(aged));
        BOOST_REQUIRE(batch.WritePaymasterProviderCommit(damaged, /*overwrite=*/true));
    }
    BOOST_CHECK(!restarted_store.ValidateProviderBudgetAuthorization(restarted_attempt,
        BudgetReservationState::SPENT, /*allow_historical_policy=*/true, error));
    {
        LOCK(restarted_wallet.cs_wallet);
        BOOST_REQUIRE(WalletBatch{restarted_wallet.GetDatabase()}.WritePaymasterProviderCommit(commit, /*overwrite=*/true));
    }

    WITH_LOCK(m_node.mempool->cs,
              m_node.mempool->removeRecursive(
                  *final_ref, MemPoolRemovalReason::CONFLICT));
    // Freeze the validation scheduler, then advance the active chain after
    // the durable commit. Recovery must finish without draining that queue.
    // Run recovery on a separate worker so the old blocking implementation
    // fails the bounded assertion but can still be released and joined.
    {
        auto lagging_database = std::make_unique<MockableDatabase>(signature_boundary_records);
        wallet::CWallet lagging_wallet{m_node.chain.get(), "post-commit-index-lag", std::move(lagging_database)};
        BOOST_REQUIRE(lagging_wallet.LoadWallet() == DBErrors::LOAD_OK);
        AlignSecurityFixtureCheckpoint(lagging_wallet.GetDatabase());
        PaymasterStore lagging_store{lagging_wallet};
        BOOST_REQUIRE_MESSAGE(lagging_store.CommitProviderFinalTransaction(request_id,
            provider_signed.attempt_id, commit, result, result.genesis_hash, error), error);
        ProviderBudgetLedger spent;
        BOOST_REQUIRE(GetPaymasterProviderBudgetLedger(lagging_wallet, spent));
        const auto spent_bytes = SerializePaymasterSecurityObject(spent);
        BOOST_REQUIRE_EQUAL(*m_node.chain->getHeight(), 0);
        const auto block = CreateBlockChain(1, Params()).front();
        std::promise<void> queue_entered, release_queue;
        auto entered = queue_entered.get_future();
        const auto release = release_queue.get_future().share();
        CallFunctionInValidationInterfaceQueue([&queue_entered, release] {
            queue_entered.set_value();
            release.wait();
        });
        entered.wait();
        auto work = std::async(std::launch::async, [&] {
            bool new_block{false};
            if (!m_node.chainman->ProcessNewBlock(block, true, true, &new_block) || !new_block) {
                throw std::runtime_error("Failed to advance tip during provider commit recovery");
            }
            return paymaster_app::RecoverProviderCommit(lagging_wallet, lagging_store, commit, commit.committed_at);
        });
        const auto ready = work.wait_for(std::chrono::seconds{5});
        release_queue.set_value();
        const auto recovered = work.get();
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK(ready == std::future_status::ready);
        BOOST_CHECK_MESSAGE(recovered.broadcast, recovered.error);
        BOOST_REQUIRE(m_node.mempool->get(final_ref->GetHash()));
        ProviderCommitRecord saved;
        BOOST_REQUIRE(lagging_store.GetProviderCommit(commit.commit_key, saved));
        BOOST_CHECK(SerializePaymasterSecurityObject(saved) == SerializePaymasterSecurityObject(commit));
        BOOST_REQUIRE(GetPaymasterProviderBudgetLedger(lagging_wallet, spent));
        BOOST_CHECK(SerializePaymasterSecurityObject(spent) == spent_bytes);
        const auto replay = paymaster_app::RecoverProviderCommit(lagging_wallet, lagging_store, commit, commit.committed_at + 1);
        BOOST_CHECK_MESSAGE(replay.broadcast, replay.error);
        BOOST_REQUIRE(GetPaymasterProviderBudgetLedger(lagging_wallet, spent));
        BOOST_CHECK(SerializePaymasterSecurityObject(spent) == spent_bytes);
        WITH_LOCK(m_node.mempool->cs, m_node.mempool->removeRecursive(*final_ref, MemPoolRemovalReason::CONFLICT));
    }

    // The public submit facade must replay existing authority while stopped,
    // including with transport disabled. The locked wallet cannot create a
    // replacement signature; exact replay must preserve every accounting row.
    wallet::CWallet facade_wallet{m_node.chain.get(), "stopped-submit-replay",
        DuplicateSecurityFixtureDatabase(restarted_wallet.GetDatabase())};
    BOOST_REQUIRE(facade_wallet.LoadWallet() == DBErrors::LOAD_OK);
    PaymasterStore facade_store{facade_wallet};
    {
        LOCK(facade_wallet.cs_wallet);
        const int height = *m_node.chain->getHeight();
        facade_wallet.SetLastBlockProcessed(height, m_node.chain->getBlockHash(height));
    }
    // Finish the existing broadcast-status result while keys are available;
    // the three calls below may only reuse that exact durable envelope.
    BOOST_REQUIRE(facade_wallet.Unlock(restart_passphrase));
    {
        CKey restored_key;
        ProviderIdentityRecord restored_identity;
        BOOST_REQUIRE_MESSAGE(GetPaymasterIdentityKey(facade_wallet, restored_key, restored_identity, error), error);
        BOOST_CHECK(restored_identity.identity_key == environment.identity.identity_key);
    }
    const auto normalized_replay = paymaster_app::RecoverProviderCommit(
        facade_wallet, facade_store, commit, std::max(GetTime(), commit.committed_at));
    BOOST_REQUIRE_MESSAGE(normalized_replay.error.empty(), normalized_replay.error);
    facade_wallet.Lock();
    BOOST_REQUIRE(facade_wallet.IsLocked());
    ProviderBudgetLedger facade_ledger;
    std::vector<ProviderPoolEntry> facade_pool;
    BOOST_REQUIRE(ReadProviderSecurityState(facade_wallet, facade_ledger, facade_pool));
    PartiallySignedTransaction replay_user_psbt;
    BOOST_REQUIRE(DecodeRawPSBT(replay_user_psbt,
        MakeByteSpan(provider_signed.user_signed_psbt), error));
    WalletContext replay_context;
    replay_context.chain = m_node.chain.get();
    replay_context.args = m_node.args;
    Manager stopped_manager{true};
    replay_context.paymaster = &stopped_manager;
    BOOST_REQUIRE(stopped_manager.StartProvider(facade_wallet.GetName(), commit.provider_id));
    paymaster_app::StopPaymaster(replay_context, facade_wallet,
        /*persistent=*/true, /*pause_setup=*/false);
    BOOST_CHECK(!stopped_manager.IsProviderRunning(facade_wallet.GetName(), commit.provider_id));
    const auto before_facade_replay = GetMockableDatabase(facade_wallet).m_records;
    ProviderFinanceEvent finance_before_replay;
    BOOST_REQUIRE(WalletBatch{facade_wallet.GetDatabase()}.ReadPaymasterFinanceEvent(
        commit.commit_key, finance_before_replay) == DatabaseReadStatus::FOUND);
    BOOST_CHECK_EQUAL(finance_before_replay.dgb_cost.value, network_fee);
    BOOST_CHECK_EQUAL(finance_before_replay.dd_income.value, fixture.response.quote.service_fee.value);
    Manager disabled_manager{false};
    for (Manager* manager : {&stopped_manager, static_cast<Manager*>(nullptr), &disabled_manager}) {
        BOOST_TEST_CONTEXT("public exact replay manager=" <<
            (manager == &stopped_manager ? "stopped" : manager ? "disabled" : "absent")) {
            replay_context.paymaster = manager;
            const auto replay = paymaster_app::SubmitPaymasterTransaction(
                replay_context, facade_wallet, replay_user_psbt, paymaster_app::ExecutionMode::MANUAL);
            BOOST_REQUIRE(replay.final_transaction);
            BOOST_CHECK(*replay.final_transaction == commit.final_transaction);
            BOOST_REQUIRE(replay.txid);
            BOOST_CHECK(*replay.txid == commit.final_txid);
            BOOST_CHECK(replay.broadcast.value_or(false));
            BOOST_CHECK(!replay.psbt);
            BOOST_CHECK(facade_wallet.IsLocked());
            ProviderCommitRecord replayed_commit;
            BOOST_REQUIRE(facade_store.GetProviderCommit(commit.commit_key, replayed_commit));
            BOOST_CHECK(SerializePaymasterSecurityObject(replayed_commit) == SerializePaymasterSecurityObject(commit));
            ProviderAttempt replayed_attempt;
            BOOST_REQUIRE(facade_store.GetAttempt(provider_signed.attempt_id, replayed_attempt));
            BOOST_CHECK(replayed_attempt.provider_signed_at == provider_signed.provider_signed_at);
            BOOST_CHECK(replayed_attempt.provider_signed_result == provider_signed.provider_signed_result);
            BOOST_CHECK(replayed_attempt.user_signed_psbt == provider_signed.user_signed_psbt);
            PaymasterResult replayed_result;
            BOOST_REQUIRE(facade_store.GetProviderResult(commit.commit_key, replayed_result));
            BOOST_CHECK(SerializePaymasterSecurityObject(replayed_result) == SerializePaymasterSecurityObject(normalized_replay.provider_result));
            ProviderBudgetLedger replayed_ledger;
            std::vector<ProviderPoolEntry> replayed_pool;
            BOOST_REQUIRE(ReadProviderSecurityState(facade_wallet, replayed_ledger, replayed_pool));
            BOOST_CHECK(SerializePaymasterSecurityObject(replayed_ledger) == SerializePaymasterSecurityObject(facade_ledger));
            BOOST_CHECK(SerializePaymasterSecurityObject(replayed_pool) == SerializePaymasterSecurityObject(facade_pool));
            ProviderFinanceEvent replayed_finance;
            BOOST_REQUIRE(WalletBatch{facade_wallet.GetDatabase()}.ReadPaymasterFinanceEvent(
                commit.commit_key, replayed_finance) == DatabaseReadStatus::FOUND);
            BOOST_CHECK(SerializePaymasterSecurityObject(replayed_finance) == SerializePaymasterSecurityObject(finance_before_replay));
            BOOST_CHECK(GetMockableDatabase(facade_wallet).m_records == before_facade_replay);
            BOOST_CHECK(!stopped_manager.IsProviderRunning(facade_wallet.GetName(), commit.provider_id));
        }
    }

    {
        LOCK(::cs_main);
        auto& coins = m_node.chainman->ActiveChainstate().CoinsTip();
        for (const CTxIn& input : final_transaction.vin) {
            BOOST_REQUIRE(coins.SpendCoin(input.prevout));
        }
    }
}

BOOST_FIXTURE_TEST_CASE(durable_observation_skips_only_unchanged_wallet_state,
                        TestChain100Setup)
{
    BOOST_REQUIRE(!g_txindex);
    struct IndexCleanup {
        ~IndexCleanup()
        {
            SyncWithValidationInterfaceQueue();
            if (g_txindex) g_txindex->Stop();
            g_txindex.reset();
        }
    } index_cleanup;
    g_txindex = std::make_unique<TxIndex>(interfaces::MakeChain(m_node), 1 << 20, true);
    BOOST_REQUIRE(g_txindex->Init());
    BOOST_REQUIRE(g_txindex->StartBackgroundSync());
    IndexWaitSynced(*g_txindex);
    wallet::CWallet wallet{m_node.chain.get(), "durable-observation", CreateMockableWalletDatabase()};
    BOOST_REQUIRE(wallet.LoadWallet() == DBErrors::LOAD_OK);
    const auto transaction = m_coinbase_txns.back();
    size_t notifications{0};
    boost::signals2::scoped_connection connection =
        wallet.NotifyTransactionChanged.connect([&](const uint256&, ChangeType) { ++notifications; });
    bool confirmed{false};
    std::string error;
    const auto observe = [&] {
        notifications = 0;
        return InsertAndBroadcastExactPaymasterTransaction(wallet, transaction, confirmed, error);
    };
    BOOST_REQUIRE_MESSAGE(observe(), error);
    BOOST_CHECK(confirmed);
    BOOST_CHECK_EQUAL(notifications, 1U);
    const TxStateConfirmed original = WITH_LOCK(wallet.cs_wallet,
        return *wallet.mapWallet.at(transaction->GetHash()).state<TxStateConfirmed>());
    const auto unchanged_database = GetMockableDatabase(wallet).m_records;
    BOOST_REQUIRE_MESSAGE(observe(), error);
    BOOST_CHECK(confirmed);
    BOOST_CHECK_EQUAL(notifications, 0U);
    BOOST_CHECK(GetMockableDatabase(wallet).m_records == unchanged_database);

    // Reconcile changed observations, including another block, a stale height,
    // a different position, and an inactive/abandoned transaction after a reorg.
    for (const TxState& changed : std::vector<TxState>{
             TxStateConfirmed{uint256S("dead"), original.confirmed_block_height, original.position_in_block},
             TxStateConfirmed{original.confirmed_block_hash, original.confirmed_block_height - 1, original.position_in_block},
             TxStateConfirmed{original.confirmed_block_hash, original.confirmed_block_height, original.position_in_block + 1},
             TxStateInactive{true}}) {
        WITH_LOCK(wallet.cs_wallet, wallet.mapWallet.at(transaction->GetHash()).m_state = changed);
        BOOST_REQUIRE_MESSAGE(observe(), error);
        BOOST_CHECK_GT(notifications, 0U);
        LOCK(wallet.cs_wallet);
        const auto* restored = wallet.mapWallet.at(transaction->GetHash()).state<TxStateConfirmed>();
        BOOST_REQUIRE(restored);
        BOOST_CHECK(restored->confirmed_block_hash == original.confirmed_block_hash);
        BOOST_CHECK_EQUAL(restored->confirmed_block_height, original.confirmed_block_height);
        BOOST_CHECK_EQUAL(restored->position_in_block, original.position_in_block);
    }
    // Identical chain state must still repair missing durable-wallet metadata.
    for (int field = 0; field < 3; ++field) {
        {
            LOCK(wallet.cs_wallet);
            auto& entry = wallet.mapWallet.at(transaction->GetHash());
            if (field == 0) entry.mapValue.erase("paymaster_durable_commit");
            if (field == 1) entry.fFromMe = false;
            if (field == 2) entry.fTimeReceivedIsTxTime = false;
        }
        BOOST_REQUIRE_MESSAGE(observe(), error);
        BOOST_CHECK_EQUAL(notifications, 1U);
        BOOST_REQUIRE_MESSAGE(observe(), error);
        BOOST_CHECK_EQUAL(notifications, 0U);
    }
    CMutableTransaction altered{*transaction};
    altered.vin.front().scriptWitness.stack.push_back({1});
    WITH_LOCK(wallet.cs_wallet, wallet.mapWallet.at(transaction->GetHash()).tx = MakeTransactionRef(altered));
    BOOST_CHECK(!observe());
    BOOST_CHECK_EQUAL(error, "PAYMASTER_FINAL_WITNESS_CONFLICT");
    BOOST_CHECK_EQUAL(notifications, 0U);
}

BOOST_FIXTURE_TEST_CASE(quote_prevout_lookup_failure_preserves_local_authority,
                        TestChain100Setup)
{
    // Readiness uses the real chain, synchronized index and loopback listeners.
    // A live synthetic coin whose transaction is absent from that index then
    // isolates lookup failure without bypassing any provider readiness gate.
    wallet::CWallet wallet{m_node.chain.get(), "quote-lookup", CreateMockableWalletDatabase()};
    BOOST_REQUIRE(wallet.LoadWallet() == DBErrors::LOAD_OK);
    ProviderSecurityEnvironment environment = MakeProviderEnvironment(GetTime(), 2000);
    environment.policy.maximum_network_fee = DGBSatoshis{COIN / 10};
    environment.settings.policy_hash = GetProviderPolicyHash(environment.policy);
    auto& runtime_limits = environment.safety.public_sponsored;
    runtime_limits.maximum_network_fee_per_transaction = DGBSatoshis{COIN / 10};
    runtime_limits.maximum_reserved_network_fee = DGBSatoshis{COIN};
    runtime_limits.maximum_network_fee_per_hour = DGBSatoshis{COIN};
    runtime_limits.maximum_network_fee_per_day = DGBSatoshis{COIN};
    environment.settings.operation_mode = ProviderOperationMode::MANUAL;
    std::string error;
    BOOST_REQUIRE_MESSAGE(AddTaprootSigningKey(wallet, environment.identity_key, error), error);
    std::vector<ProviderPoolEntry> pool;
    for (size_t i = 0; i < REQUIRED_ADMISSION_SLOTS; ++i) {
        const CBlock block = CreateAndProcessBlock({}, environment.identity.identity_script);
        const int height = *m_node.chain->getHeight();
        {
            LOCK(wallet.cs_wallet);
            BOOST_REQUIRE(wallet.AddToWallet(block.vtx.front(), TxStateConfirmed{block.GetHash(), height, 0}));
        }
        ProviderPoolEntry entry;
        entry.outpoint = COutPoint{block.vtx.front()->GetHash(), 0};
        entry.purpose = PoolPurpose::ADMISSION;
        entry.asset = PoolAsset::DGB;
        entry.state = PoolEntryState::AVAILABLE;
        entry.script_pub_key = block.vtx.front()->vout.front().scriptPubKey;
        entry.dgb_value = DGBSatoshis{block.vtx.front()->vout.front().nValue};
        entry.confirmation_height = height;
        entry.updated_at = GetTime();
        pool.push_back(entry);
        SetMockTime(GetTime() + 1);
    }
    const int64_t now{GetTime()};
    const uint256 seed{uint256S("1811")};
    ProviderQuoteFixture fixture;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(environment,
        "550e8400-e29b-41d4-a716-446655441097", seed, now, fixture, error,
        nullptr, {}, DGBSatoshis{500}, 5500, Params().GenesisBlock().GetHash()), error);
    // The capacity-bound operational slot is reserved, as for a real quote
    // continuation. No new capacity or budget may be granted in this test.
    fixture.pool_entry.confirmation_height = 0;
    pool.push_back(fixture.pool_entry);
    BOOST_REQUIRE(PersistProviderQuoteEnvironment(wallet, environment, pool));
    {
        LOCK(wallet.cs_wallet);
        wallet.SetLastBlockProcessed(*m_node.chain->getHeight(),
                                    m_node.chain->getBlockHash(*m_node.chain->getHeight()));
    }

    BOOST_REQUIRE(!g_txindex);
    struct ServicesCleanup {
        CConnman& connman;
        ~ServicesCleanup()
        {
            connman.Interrupt();
            connman.Stop();
            if (g_txindex) g_txindex->Interrupt();
            SyncWithValidationInterfaceQueue();
            if (g_txindex) g_txindex->Stop();
            g_txindex.reset();
        }
    } services_cleanup{*m_node.connman};
    g_txindex = std::make_unique<TxIndex>(interfaces::MakeChain(m_node), 1 << 20, true);
    BOOST_REQUIRE(g_txindex->Init());
    BOOST_REQUIRE(g_txindex->StartBackgroundSync());
    IndexWaitSynced(*g_txindex);
    m_node.args->ForceSetArg("-txindex", "1");
    m_node.args->ForceSetArg("-paymasterendpoint", "127.0.0.1:18445");
    m_node.args->ForceSetArg("-dnsseed", "0");
    m_node.args->ForceSetArg("-dandelion", "0");
    CConnman::Options options{};
    options.nLocalServices = ServiceFlags{NODE_NETWORK | NODE_WITNESS | NODE_P2P_V2};
    options.nMaxConnections = 8;
    // The addnode thread requires an interruptible semaphore even with no
    // configured peers. Zero permits would also leave cleanup unable to join.
    options.nMaxAddnode = 1;
    options.paymaster_inbound = 2;
    options.m_msgproc = m_node.peerman.get();
    options.m_use_addrman_outgoing = false;
    options.vBinds = {LookupNumeric("127.0.0.1:0")};
    options.paymaster_binds = {{LookupNumeric("127.0.0.1:0"), false}};
    BOOST_REQUIRE(m_node.connman->Start(*m_node.scheduler, options));

    WalletContext context;
    context.chain = m_node.chain.get();
    context.args = m_node.args;
    Manager manager{true};
    context.paymaster = &manager;
    BOOST_REQUIRE(manager.StartProvider(wallet.GetName(), environment.identity.provider_id));
    const auto readiness = paymaster_app::GetProviderReadiness(wallet, context);
    BOOST_REQUIRE_EQUAL(readiness.errors.size(), 1U);
    BOOST_REQUIRE_EQUAL(readiness.errors.front(), "PAYMASTER_OPERATIONAL_SLOT_MISSING");
    BOOST_REQUIRE(paymaster_app::CapacityContinuationMayProceed(readiness));
    BOOST_REQUIRE(paymaster_app::ProviderTxIndexIsReady(wallet, false));
    BOOST_REQUIRE_MESSAGE(ValidateQuoteRequestEnvelope(fixture.request,
        Params().GenesisBlock().GetHash(), now, error), error);

    const COutPoint outpoint{fixture.request.intent.user_dd_inputs.front()};
    uint256 block_hash;
    CTransactionRef creating_tx;
    BOOST_REQUIRE(!g_txindex->FindTx(outpoint.hash, block_hash, creating_tx));
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(outpoint,
            Coin{CTxOut{fixture.user_transaction->vout.at(outpoint.n)}, 1, false}, false);
    }
    const auto before = GetMockableDatabase(wallet).m_records;
    const uint256 message_id{SecurityTestId(seed, 20)};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(44, message_id, 100,
        DirectPayload{fixture.request}, now, 44, {1, 8, 8}));
    BOOST_CHECK_EXCEPTION(paymaster_app::ProcessPaymasterRequests(
        context, wallet, paymaster_app::ExecutionMode::MANUAL), ApplicationError,
        [](const ApplicationError& failure) {
            return failure.kind == ErrorKind::WALLET &&
                   std::string{failure.what()} == "PAYMASTER_USER_PREVOUT_LOOKUP_FAILED";
        });
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);

    // The exact queued peer request becomes permanently rejectable only when
    // canonical chainstate also proves that its input is missing or spent.
    {
        LOCK(cs_main);
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().CoinsTip().SpendCoin(outpoint));
    }
    const auto result = paymaster_app::ProcessPaymasterRequests(
        context, wallet, paymaster_app::ExecutionMode::MANUAL);
    BOOST_CHECK(result.HasMessage());
    BOOST_CHECK(result.IsRejected());
    BOOST_CHECK(!result.queued.value_or(false));
    BOOST_CHECK_EQUAL(result.message_type.value_or(""), "quote");
    BOOST_CHECK_EQUAL(result.rejection_reason.value_or(""), "PAYMASTER_USER_PREVOUT_NOT_FOUND");
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
    BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);
    BOOST_CHECK(manager.IsProviderRunning(wallet.GetName(), environment.identity.provider_id));
}

BOOST_FIXTURE_TEST_CASE(quote_request_conflict_is_consumed,
                        QuoteRequestProcessingSetup)
{
    CheckClassification(ConflictingRequest(), false, "PAYMASTER_REQUEST_ID_CONFLICT");
    auto changed = fixture.request;
    changed.intent.client_nonce = SecurityTestId(seed, 31);
    CheckClassification(changed, false, "PAYMASTER_REQUEST_ID_CONFLICT");
    changed = fixture.request;
    ++changed.intent.recipient_amount.value;
    CheckClassification(changed, false, "PAYMASTER_REQUEST_ID_CONFLICT");
}

BOOST_FIXTURE_TEST_CASE(quote_request_collision_preserves_local_corruption,
                        QuoteRequestProcessingSetup)
{
    PaymasterStore store{wallet};
    PaymentSession session;
    BOOST_REQUIRE(store.GetSessionByRequestId(fixture.request.intent.request_id, session));
    const auto session_key = std::make_pair(DBKeys::PAYMASTER_SESSION, session.request_id);
    const auto attempt_key = std::make_pair(DBKeys::PAYMASTER_ATTEMPT, fixture.attempt.attempt_id);
    auto batch = wallet.GetDatabase().MakeBatch();
    const auto incoming = ConflictingRequest();
    const auto check_corrupt = [&] {
        CheckClassification(incoming, true, "PAYMASTER_PERSISTED_QUOTE_CORRUPT");
    };

    BOOST_REQUIRE(batch->Erase(attempt_key));
    check_corrupt();
    BOOST_REQUIRE(batch->Write(attempt_key, fixture.attempt));

    for (int field = 0; field < 9; ++field) {
        BOOST_TEST_CONTEXT("corrupt provider attempt field " << field) {
            auto changed = fixture.attempt;
            if (field == 0) changed.signed_quote = {1};
            if (field == 1) changed.signed_quote.push_back(0);
            if (field == 2) changed.unsigned_psbt = {1};
            if (field == 3) changed.session_id = SecurityTestId(seed, 32);
            if (field == 4) changed.provider_id = SecurityTestId(seed, 33);
            if (field == 5) changed.client_nonce = SecurityTestId(seed, 34);
            if (field == 6) changed.intent_hash = SecurityTestId(seed, 35);
            if (field == 7) changed.created_at = changed.quote_expires_at;
            if (field == 8) ++changed.version;
            BOOST_REQUIRE(batch->Write(attempt_key, changed));
            check_corrupt();
            BOOST_REQUIRE(batch->Write(attempt_key, fixture.attempt));
        }
    }
    for (int field = 0; field < 3; ++field) {
        BOOST_TEST_CONTEXT("corrupt provider session field " << field) {
            auto changed = session;
            if (field == 0) changed.attempt_ids.clear();
            if (field == 1) changed.session_id = SecurityTestId(seed, 36);
            if (field == 2) changed.canonical_request_hash = SecurityTestId(seed, 37);
            BOOST_REQUIRE(batch->Write(session_key, changed));
            check_corrupt();
            BOOST_REQUIRE(batch->Write(session_key, session));
        }
    }
    auto unsupported = session;
    ++unsupported.version;
    BOOST_REQUIRE(batch->Write(session_key, unsupported));
    CheckClassification(incoming, true, "PAYMASTER_SESSION_DATABASE_READ");
    BOOST_REQUIRE(batch->Write(session_key, uint8_t{0}));
    CheckClassification(incoming, true, "PAYMASTER_SESSION_DATABASE_READ");
    BOOST_REQUIRE(batch->Write(session_key, session));
}

BOOST_FIXTURE_TEST_CASE(quote_request_collision_validates_saved_quote_authority,
                        QuoteRequestProcessingSetup)
{
    const auto attempt_key = std::make_pair(DBKeys::PAYMASTER_ATTEMPT, fixture.attempt.attempt_id);
    auto batch = wallet.GetDatabase().MakeBatch();
    const std::array<const char*, 5> fields{
        "signature bit", "identity key", "decodable PSBT", "input roles", "quote envelope"};
    for (size_t field = 0; field < fields.size(); ++field) {
        BOOST_TEST_CONTEXT(fields[field]) {
            auto changed = fixture.attempt;
            auto response = fixture.response;
            std::string error;
            if (field == 0) {
                BOOST_REQUIRE_EQUAL(response.quote.identity_signature.size(), 64U);
                response.quote.identity_signature.front() ^= 1;
                changed.signed_quote = SerializePaymasterSecurityObject(response);
            }
            if (field == 1) {
                CKey other_identity;
                other_identity.MakeNewKey(true);
                changed.provider_identity_key = XOnlyPubKey{other_identity.GetPubKey()};
                BOOST_REQUIRE(changed.provider_identity_key.IsFullyValid());
                BOOST_REQUIRE(GetPaymasterId(changed.provider_identity_key) != changed.provider_id);
            }
            if (field == 2) {
                PartiallySignedTransaction psbt;
                BOOST_REQUIRE_MESSAGE(DecodeRawPSBT(psbt, MakeByteSpan(changed.unsigned_psbt), error), error);
                BOOST_REQUIRE(psbt.tx);
                ++psbt.tx->nLockTime;
                changed.unsigned_psbt = SerializePaymasterSecurityObject(psbt);
            }
            if (field == 3) {
                BOOST_REQUIRE_EQUAL(changed.input_roles.size(), 2U);
                changed.input_roles.front() = ReservationRole::USER_DGB;
            }
            if (field == 4) {
                CollaborativePSBTTemplate trusted;
                BOOST_REQUIRE_MESSAGE(DecodeRawPSBT(trusted.psbt,
                    MakeByteSpan(changed.unsigned_psbt), error), error);
                trusted.input_roles = {InputRole::USER_DD, InputRole::PROVIDER_DGB};
                // Preserve both signature and template binding, leaving only
                // the existing envelope rule (rate divisible by ten) invalid.
                response.quote.fee_rate_bps = 1;
                response.quote.template_commitment = GetCollaborativeTemplateCommitment(
                    fixture.request.intent, response.quote, trusted);
                BOOST_REQUIRE(environment.identity_key.SignSchnorr(
                    GetPaymasterQuoteSignatureHash(response.quote),
                    response.quote.identity_signature, nullptr, uint256{}));
                changed.template_commitment = response.quote.template_commitment;
                changed.commit_key = GetPaymasterCommitKey(changed.provider_id,
                    changed.client_nonce, changed.intent_hash, changed.quote_id,
                    changed.template_commitment);
                changed.signed_quote = SerializePaymasterSecurityObject(response);
                BOOST_REQUIRE(changed.provider_identity_key.VerifySchnorr(
                    GetPaymasterQuoteSignatureHash(response.quote),
                    response.quote.identity_signature));
                BOOST_REQUIRE(!ValidateQuoteResponseEnvelope(response,
                    Params().GenesisBlock().GetHash(), response.quote.created_at, error));
            }
            BOOST_REQUIRE(batch->Write(attempt_key, changed));
            CheckClassification(ConflictingRequest(), true, "PAYMASTER_PERSISTED_QUOTE_CORRUPT");
            BOOST_REQUIRE(batch->Write(attempt_key, fixture.attempt));
        }
    }
}

BOOST_FIXTURE_TEST_CASE(quote_request_historical_provider_uses_saved_identity,
                        QuoteRequestProcessingSetup)
{
    auto previous_environment = MakeProviderEnvironment(now, 2000);
    ProviderQuoteFixture previous;
    std::string error;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(previous_environment,
        fixture.request.intent.request_id, seed, now, previous, error,
        nullptr, {}, DGBSatoshis{500}, 5500, Params().GenesisBlock().GetHash(),
        /*intent_ttl=*/60, /*quote_ttl=*/20), error);
    BOOST_REQUIRE(previous.attempt.provider_id != environment.identity.provider_id);
    BOOST_REQUIRE(previous.attempt.attempt_id == fixture.attempt.attempt_id);
    BOOST_REQUIRE(previous.attempt.session_id == fixture.attempt.session_id);
    BOOST_REQUIRE_MESSAGE(ValidateQuoteResponseEnvelope(previous.response,
        Params().GenesisBlock().GetHash(), previous.response.quote.created_at, error), error);
    BOOST_REQUIRE(previous.attempt.provider_identity_key.VerifySchnorr(
        GetPaymasterQuoteSignatureHash(previous.response.quote),
        previous.response.quote.identity_signature));

    // Exercise the classification of independently signed historical artifacts
    // with a different current provider route, not the rotation workflow itself.
    PaymasterStore store{wallet};
    PaymentSession session;
    BOOST_REQUIRE(store.GetSessionByRequestId(fixture.request.intent.request_id, session));
    session.canonical_request_hash = Hash(previous.attempt.quote_request);
    session.user_inputs = previous.request.intent.user_dd_inputs;
    auto batch = wallet.GetDatabase().MakeBatch();
    BOOST_REQUIRE(batch->Write(std::make_pair(DBKeys::PAYMASTER_ATTEMPT,
        previous.attempt.attempt_id), previous.attempt));
    BOOST_REQUIRE(batch->Write(std::make_pair(DBKeys::PAYMASTER_SESSION,
        session.request_id), session));
    CheckClassification(fixture.request, false, "PAYMASTER_REQUEST_ID_CONFLICT");
}

BOOST_FIXTURE_TEST_CASE(quote_request_client_session_and_tombstone_are_consumed,
                        QuoteRequestProcessingSetup)
{
    const auto session_key = std::make_pair(DBKeys::PAYMASTER_SESSION, fixture.request.intent.request_id);
    auto batch = wallet.GetDatabase().MakeBatch();
    PaymentSession client;
    client.request_id = fixture.request.intent.request_id;
    client.session_id = SecurityTestId(seed, 38);
    client.canonical_request_hash = SecurityTestId(seed, 39);
    client.created_at = now;
    client.updated_at = now;
    BOOST_REQUIRE(!client.provider_side);
    BOOST_REQUIRE(batch->Write(session_key, client));
    CheckClassification(ConflictingRequest(), false, "PAYMASTER_REQUEST_ID_CONFLICT");

    IdempotencyTombstone tombstone;
    tombstone.request_id = client.request_id;
    tombstone.session_id = client.session_id;
    tombstone.canonical_request_hash = client.canonical_request_hash;
    tombstone.final_state = SessionState::CANCELED_SAFE;
    BOOST_REQUIRE(batch->Erase(session_key));
    BOOST_REQUIRE(WalletBatch{wallet.GetDatabase()}.WritePaymasterTombstone(tombstone));
    CheckClassification(ConflictingRequest(), false, "PAYMASTER_REQUEST_ID_CONFLICT");

    const auto tombstone_key = std::make_pair(DBKeys::PAYMASTER_TOMBSTONE, tombstone.request_id);
    tombstone.session_id.SetNull();
    BOOST_REQUIRE(batch->Write(tombstone_key, tombstone));
    CheckClassification(ConflictingRequest(), true, "PAYMASTER_SESSION_DATABASE_READ");
}

BOOST_FIXTURE_TEST_CASE(expired_quote_retry_is_consumed,
                        QuoteRequestProcessingSetup)
{
    SetMockTime(fixture.response.quote.expires_at);
    // Allow normal expiry reconciliation to finish before comparing the
    // records. The incoming retry must not perform another financial action.
    paymaster_app::GetProviderReadiness(wallet, context);
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateQuoteRequestEnvelope(fixture.request,
        Params().GenesisBlock().GetHash(), GetTime(), error), error);
    CheckClassification(fixture.request, false, "PAYMASTER_PROVIDER_RETRY_EXPIRED");
}

BOOST_FIXTURE_TEST_CASE(identical_quote_retry_reuses_signed_response,
                        QuoteRequestProcessingSetup)
{
    const auto before = GetMockableDatabase(wallet).m_records;
    const auto checkpoint_before = CheckpointBytes();
    for (int retry = 0; retry < 2; ++retry) {
        // A restarted transport has no live replay slot. Persisted authority
        // must still reproduce the exact response without a new reservation.
        Manager transport{true};
        context.paymaster = &transport;
        BOOST_REQUIRE(transport.StartProvider(wallet.GetName(), environment.identity.provider_id));
        EnqueueQuote(transport, fixture.request);
        const auto result = paymaster_app::ProcessPaymasterRequests(
            context, wallet, paymaster_app::ExecutionMode::MANUAL);
        BOOST_CHECK(result.HasMessage());
        BOOST_CHECK(!result.IsRejected());
        BOOST_CHECK(result.queued.value_or(false));
        BOOST_CHECK_EQUAL(transport.DirectMessageCount(), 0U);
        const auto outbound = transport.TakeOutboundDirectMessages(44, 1, GetTime());
        BOOST_REQUIRE_EQUAL(outbound.size(), 1U);
        const auto* response = std::get_if<PaymasterQuoteResponse>(&outbound.front().payload);
        BOOST_REQUIRE(response);
        BOOST_CHECK(SerializePaymasterSecurityObject(*response) == fixture.attempt.signed_quote);
        BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);
        BOOST_CHECK(CheckpointBytes() == checkpoint_before);
        context.paymaster = &manager;
    }
}

BOOST_FIXTURE_TEST_CASE(automatic_quote_rejection_allows_following_work,
                        QuoteRequestProcessingSetup)
{
    environment.settings.operation_mode = ProviderOperationMode::AUTOMATIC;
    BOOST_REQUIRE(WalletBatch{wallet.GetDatabase()}.WritePaymasterSettings(environment.settings));
    // The reserved slot is a pending continuation, so the actual service loop
    // must process existing requests/submits while maintenance waits for it.
    RunPaymasterProviderServiceCycle(context, wallet);
    const auto initial_status = manager.GetProviderServiceStatus(wallet.GetName());
    BOOST_REQUIRE(initial_status.state == ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION);

    EnqueueQuote(manager, ConflictingRequest());
    PaymasterSubmit stale;
    stale.genesis_hash = Params().GenesisBlock().GetHash();
    stale.request_id = "550e8400-e29b-41d4-a716-446655441099";
    stale.session_id = SecurityTestId(seed, 40);
    stale.provider_id = environment.identity.provider_id;
    stale.quote_id = SecurityTestId(seed, 41);
    stale.commit_key = SecurityTestId(seed, 42);
    stale.template_commitment = SecurityTestId(seed, 43);
    stale.user_psbt = {1};
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateSubmitEnvelope(stale, stale.genesis_hash, error), error);
    const auto submit_bytes = SerializePaymasterSecurityObject(stale);
    BOOST_REQUIRE(manager.EnqueueNetworkDirectMessage(45, Hash(submit_bytes), submit_bytes.size(),
        DirectPayload{stale}, GetTime(), 45, {1, 9, 9},
        DirectAdmissionClass::REQUEST) == DirectEnqueueResult::ACCEPTED);
    const auto before = GetMockableDatabase(wallet).m_records;
    const auto checkpoint_before = CheckpointBytes();
    RunPaymasterProviderServiceCycle(context, wallet);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
    BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 0U);
    BOOST_CHECK(manager.GetProviderServiceStatus(wallet.GetName()).state == initial_status.state);
    BOOST_CHECK_EQUAL(manager.GetProviderServiceStatus(wallet.GetName()).last_error, initial_status.last_error);
    BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);
    BOOST_CHECK(CheckpointBytes() == checkpoint_before);

    EnqueueQuote(manager, fixture.request);
    RunPaymasterProviderServiceCycle(context, wallet);
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
    BOOST_CHECK(manager.GetProviderServiceStatus(wallet.GetName()).state == initial_status.state);
    const auto outbound = manager.TakeOutboundDirectMessages(44, 1, GetTime());
    BOOST_REQUIRE_EQUAL(outbound.size(), 1U);
    const auto* response = std::get_if<PaymasterQuoteResponse>(&outbound.front().payload);
    BOOST_REQUIRE(response);
    BOOST_CHECK(SerializePaymasterSecurityObject(*response) == fixture.attempt.signed_quote);
    BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);
    BOOST_CHECK(CheckpointBytes() == checkpoint_before);
}

BOOST_FIXTURE_TEST_CASE(client_recovery_read_failure_stops_new_preparation,
                        ClientRecoveryReadSetup)
{
    std::string error;
    BOOST_REQUIRE_MESSAGE(paymaster_app::CheckPaymasterClientReadiness(wallet, context, error), error);
    PaymasterStore store{wallet};
    PaymentSession session;
    BOOST_REQUIRE(store.CreateOrJoinSession("550e8400-e29b-41d4-a716-446655441099",
        SecurityTestId(seed, 120), FeeMode::PAYMASTER, now, session, error) == CreatePaymasterSessionResult::CREATED);
    const paymaster_app::ClientRecoveryOptions options;
    const auto invoke = [&] {
        return paymaster_app::ResolveAlternativePaymasterRecovery(context, wallet, store, session, fixture.attempt, options);
    };
    // True absence still enters the existing new-recovery eligibility gate.
    BOOST_CHECK_EXCEPTION(invoke(), ApplicationError, [](const ApplicationError& failure) {
        return failure.code == "PAYMASTER_ALTERNATIVE_RECOVERY_NOT_ALLOWED";
    });
    auto& database = GetMockableDatabase(wallet);
    const auto absent_rows = database.m_records;
    const uint256 recovery_id = SecurityTestId(seed, 121);
    for (int scenario = 0; scenario < 3; ++scenario) {
        BOOST_TEST_CONTEXT("client recovery read scenario " << scenario) {
            database.m_records = absent_rows;
            {
                LOCK(wallet.cs_wallet);
                auto batch = wallet.GetDatabase().MakeBatch();
                BOOST_REQUIRE(batch->Write(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_REQUEST, session.request_id), recovery_id));
                if (scenario != 2) {
                    BOOST_REQUIRE(batch->Write(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, recovery_id),
                        uint16_t(AlternativeRecoveryRecord::CURRENT_VERSION + scenario)));
                }
            }
            const auto before = database.m_records;
            const auto checkpoint_before = CheckpointBytes();
            const size_t messages_before = manager.DirectMessageCount();
            const size_t outbound_before = manager.OutboundDirectMessageCount();
            const char* expected = scenario == 1 ? "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION" : "PAYMASTER_RECOVERY_DATABASE_READ";
            BOOST_CHECK_EXCEPTION(invoke(), ApplicationError, [&](const ApplicationError& failure) {
                return failure.code == expected && failure.kind == ErrorKind::WALLET &&
                    failure.disposition == ErrorDisposition::RECONCILE && !failure.session_absent;
            });
            BOOST_CHECK(database.m_records == before);
            BOOST_CHECK(CheckpointBytes() == checkpoint_before);
            BOOST_CHECK_EQUAL(manager.DirectMessageCount(), messages_before);
            BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), outbound_before);
        }
    }
    // A readable record still reaches the existing continuation checks.
    database.m_records = absent_rows;
    auto request = MakeRecoveryReadRequest(environment, seed, now);
    request.request_id = session.request_id;
    request.session_id = session.session_id;
    request.capacity_request.session_id = session.session_id;
    auto record = MakeRecoveryReadReplay(environment, request, now);
    record.provider_side = false;
    record.expired = true;
    {
        LOCK(wallet.cs_wallet);
        WalletBatch batch{wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAlternativeRecovery(record));
        BOOST_REQUIRE(batch.WritePaymasterAlternativeRecoveryRequest(session.request_id, record.recovery_id));
    }
    const auto before = database.m_records;
    BOOST_CHECK_EXCEPTION(invoke(), ApplicationError, [](const ApplicationError& failure) {
        return failure.code == "PAYMASTER_ALTERNATIVE_RECOVERY_CONFLICT" && failure.kind == ErrorKind::INVALID_INPUT;
    });
    BOOST_CHECK(database.m_records == before);
}

static void ExerciseFirstProviderRecoveryCommit(node::NodeContext& node,
    AlternativeRecoveryRecord recovery, const CKey& provider_key,
    const CTransactionRef& funding, const PaymasterResult& result, int64_t now,
    ClientRecoveryReadSetup* chain_setup = nullptr, const TxStateConfirmed* confirmed_state = nullptr)
{
    wallet::CWallet provider{node.chain.get(), "first-provider-recovery", CreateMockableWalletDatabase()};
    BOOST_REQUIRE(provider.LoadWallet() == DBErrors::LOAD_OK);
    std::string error;
    BOOST_REQUIRE(AddTaprootSigningKey(provider, provider_key, error));
    {
        LOCK(provider.cs_wallet);
        BOOST_REQUIRE(provider.AddToWallet(funding, TxStateInactive{}));
    }
    auto environment = MakeProviderEnvironment(now, 2 * COIN);
    auto& limits = environment.safety.user_paid;
    limits = SecurityLimits(2 * COIN);
    limits.maximum_network_fee_per_transaction = DGBSatoshis{COIN};
    limits.maximum_network_fee_per_hour = DGBSatoshis{2 * COIN};
    limits.maximum_network_fee_per_day = DGBSatoshis{2 * COIN};
    const auto& manifest = recovery.recovery_response.manifest;
    recovery.provider_side = true;
    recovery.accepted_recovery_authorization_commitment.SetNull();
    recovery.recovery_authorization_accepted_at = 0;
    recovery.provider_safety_policy_hash = GetProviderSafetyPolicyHash(environment.safety);
    recovery.provider_budget_reservation_id = result.commit_key;
    recovery.provider_netgroup_bucket = environment.netgroup_bucket;
    recovery.provider_maximum_network_fee = manifest.network_fee;
    BOOST_REQUIRE_MESSAGE(ReserveProviderBudget(environment.ledger, environment.safety,
        FundingModel::USER_PAID, SponsorshipScope::PUBLIC, result.commit_key,
        manifest.network_fee, GetRecipientBudgetBucket(environment.ledger, manifest.wallet_returns.front().script_pub_key),
        now, error, environment.netgroup_bucket), error);
    ProviderIdentityRecord identity;
    identity.provider_id = recovery.recovery_provider_id;
    identity.identity_key = recovery.recovery_provider_identity_key;
    identity.identity_script = manifest.recovery_provider_dgb_change_scripts.front();
    identity.display_name = "First Recovery Provider";
    identity.created_at = now - 1;
    ProviderPoolEntry input;
    input.outpoint = manifest.recovery_provider_dgb_inputs.front();
    input.asset = PoolAsset::DGB;
    input.state = PoolEntryState::RESERVED;
    input.script_pub_key = funding->vout[input.outpoint.n].scriptPubKey;
    input.dgb_value = DGBSatoshis{funding->vout[input.outpoint.n].nValue};
    input.confirmation_height = 1;
    input.reservation_id = result.commit_key;
    input.updated_at = now;
    {
        LOCK(provider.cs_wallet);
        WalletBatch batch{provider.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterIdentity(identity));
        BOOST_REQUIRE(batch.WritePaymasterAlternativeRecovery(recovery));
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(environment.ledger));
        BOOST_REQUIRE(batch.WritePaymasterProviderPool({input}));
    }
    recovery.phase = AlternativeRecoveryPhase::FINAL_COMMITTED;
    recovery.final_transaction = SerializePaymasterSecurityObject(*result.final_transaction);
    recovery.expected_wtxid = CTransaction{*result.final_transaction}.GetWitnessHash();
    recovery.signed_result = result;
    ProviderCommitRecord commit;
    commit.commit_key = result.commit_key;
    commit.provider_id = identity.provider_id;
    commit.quote_id = recovery.recovery_id;
    commit.template_commitment = manifest.template_commitment;
    commit.final_txid = *result.txid;
    commit.raw_transaction_hash = Hash(recovery.final_transaction);
    commit.final_transaction = recovery.final_transaction;
    commit.provider_inputs = {input.outpoint};
    commit.committed_at = now;
    PaymasterStore store{provider};
    auto& database = GetMockableDatabase(provider);
    const auto before = database.m_records;
    BOOST_REQUIRE(!provider.IsWalletFlagSet(WALLET_FLAG_PAYMASTER_AUTHORIZATION));
    DataStream encoded_flags;
    encoded_flags << DBKeys::FLAGS;
    database.m_refuse_write = [&](Span<const std::byte> key) {
        return key.size() == encoded_flags.size() && std::equal(key.begin(), key.end(), encoded_flags.begin());
    };
    BOOST_CHECK(!store.CommitProviderAlternativeRecoveryFinal(recovery, commit, result,
        result.genesis_hash, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_WRITE");
    database.m_refuse_write = {};
    BOOST_CHECK(database.m_records == before);
    BOOST_CHECK(!provider.IsWalletFlagSet(WALLET_FLAG_PAYMASTER_AUTHORIZATION));
    database.FailCommit();
    BOOST_CHECK(!store.CommitProviderAlternativeRecoveryFinal(recovery, commit, result,
        result.genesis_hash, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_DATABASE_COMMIT");
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before);
    BOOST_CHECK(!provider.IsWalletFlagSet(WALLET_FLAG_PAYMASTER_AUTHORIZATION));
    AlignSecurityFixtureCheckpoint(database);
    BOOST_REQUIRE_MESSAGE(store.CommitProviderAlternativeRecoveryFinal(recovery, commit, result,
        result.genesis_hash, error), error);
    BOOST_CHECK(provider.IsWalletFlagSet(WALLET_FLAG_PAYMASTER_AUTHORIZATION));
    uint64_t flags{0};
    BOOST_REQUIRE(database.MakeBatch()->Read(DBKeys::FLAGS, flags));
    BOOST_CHECK((flags & WALLET_FLAG_PAYMASTER_AUTHORIZATION) != 0);
    const auto committed = database.m_records;
    database.FailWriteAt(0);
    BOOST_REQUIRE_MESSAGE(store.CommitProviderAlternativeRecoveryFinal(recovery, commit, result,
        result.genesis_hash, error), error);
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == committed);
    wallet::CWallet reopened{node.chain.get(), "first-provider-recovery-reopened",
        DuplicateSecurityFixtureDatabase(database)};
    BOOST_REQUIRE(reopened.LoadWallet() == DBErrors::LOAD_OK);
    BOOST_CHECK(reopened.IsWalletFlagSet(WALLET_FLAG_PAYMASTER_AUTHORIZATION));
    // Exact recovery replay also repairs the missing flag in a wallet image
    // produced by the old alternative-recovery path, without spending twice.
    const uint64_t legacy_flags = flags & ~WALLET_FLAG_PAYMASTER_AUTHORIZATION;
    BOOST_REQUIRE(reopened.GetDatabase().MakeBatch()->Write(DBKeys::FLAGS, legacy_flags));
    BOOST_REQUIRE(reopened.LoadWalletFlags(legacy_flags));
    BOOST_REQUIRE_MESSAGE(PaymasterStore{reopened}.CommitProviderAlternativeRecoveryFinal(
        recovery, commit, result, result.genesis_hash, error), error);
    BOOST_CHECK(reopened.IsWalletFlagSet(WALLET_FLAG_PAYMASTER_AUTHORIZATION));
    ProviderBudgetLedger repaired_budget;
    BOOST_REQUIRE(WalletBatch{reopened.GetDatabase()}.ReadPaymasterProviderBudgetLedger(repaired_budget));
    BOOST_REQUIRE_EQUAL(repaired_budget.reservations.size(), 1U);
    BOOST_CHECK(repaired_budget.reservations.front().state == BudgetReservationState::SPENT);

    if (!chain_setup) return;
    // The flag-repair clone above owns the current independent checkpoint.
    wallet::CWallet& history_wallet = reopened;
    auto& history_database = GetMockableDatabase(history_wallet);
    PaymasterStore history_store{history_wallet};
    BOOST_REQUIRE(confirmed_state);
    const auto final_transaction = MakeTransactionRef(*result.final_transaction);
    {
        LOCK(history_wallet.cs_wallet);
        BOOST_REQUIRE(history_wallet.AddToWallet(final_transaction, *confirmed_state));
        history_wallet.SetLastBlockProcessed(confirmed_state->confirmed_block_height,
            confirmed_state->confirmed_block_hash);
    }

    // Reuse real signed recovery evidence and its actually mined final. Only
    // the unrelated history fillers are synthetic; no signature, mempool or
    // consensus validation is bypassed for the completed recovery itself.
    environment.identity_key = provider_key;
    environment.identity = identity;
    environment.policy.maximum_network_fee = DGBSatoshis{COIN};
    environment.settings.policy_hash = GetProviderPolicyHash(environment.policy);
    environment.netgroup_bucket = GetNetgroupBudgetBucket(environment.ledger, {1, 2, 3});
    std::vector<ProviderPoolEntry> retained_pool;
    BOOST_REQUIRE(ReadProviderSecurityState(history_wallet, environment.ledger, retained_pool));
    ProviderQuoteFixture next_quote;
    BOOST_REQUIRE_MESSAGE(BuildProviderQuoteFixture(environment,
        "550e8400-e29b-41d4-a716-446655449999", uint256S("1931"), now,
        next_quote, error, nullptr, {}, DGBSatoshis{500}, 5500, result.genesis_hash), error);
    retained_pool.push_back(next_quote.pool_entry);
    {
        // The recovery fixture already enrolled this immutable identity.
        LOCK(history_wallet.cs_wallet);
        WalletBatch batch{history_database};
        BOOST_REQUIRE(batch.TxnBegin());
        BOOST_REQUIRE(batch.WritePaymasterPolicy(environment.policy));
        BOOST_REQUIRE(batch.WritePaymasterSettings(environment.settings));
        BOOST_REQUIRE(batch.WritePaymasterProviderSafetyPolicy(environment.safety));
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(environment.ledger));
        BOOST_REQUIRE(batch.WritePaymasterProviderPool(retained_pool));
        for (const auto& record : environment.capacity_records) {
            BOOST_REQUIRE(batch.WritePaymasterCapacityNonce(record.client_nonce, record.request_hash));
            BOOST_REQUIRE(batch.WritePaymasterCapacitySession(record.session_key, record.request_hash));
            BOOST_REQUIRE(batch.WritePaymasterCapacityResponse(record.request_hash, record.proof));
        }
        BOOST_REQUIRE(batch.TxnCommit());
    }
    {
        LOCK(history_wallet.cs_wallet);
        WalletBatch batch{history_database};
        BOOST_REQUIRE(batch.TxnBegin());
        for (size_t i = 0; i < paymaster_store::internal::MAX_RETAINED_PROVIDER_SESSIONS - 1; ++i) {
            PaymentSession filler;
            filler.provider_side = true;
            filler.request_id = strprintf("550e8400-e29b-41d4-a716-%012u", i);
            filler.session_id = SecurityTestId(uint256S("1932"), i + 1);
            filler.canonical_request_hash = uint256::ONE;
            filler.created_at = filler.updated_at = now;
            BOOST_REQUIRE(batch.WritePaymasterSession(filler));
        }
        BOOST_REQUIRE(batch.TxnCommit());
    }
    const auto admission = [&] {
        uint256 netgroup;
        return history_store.CheckProviderQuoteAdmission(next_quote.request,
            Hash(SerializePaymasterSecurityObject(next_quote.request)), {1, 2, 3},
            false, DGBSatoshis{500}, now, netgroup, error);
    };
    const auto update_tip = [&] {
        IndexWaitSynced(*g_txindex);
        const auto height = *node.chain->getHeight();
        const auto hash = node.chain->getBlockHash(height);
        LOCK(history_wallet.cs_wallet);
        history_wallet.SetLastBlockProcessed(height, hash);
    };
    BOOST_CHECK(!admission());
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_HISTORY_FULL");
    for (int depth = 1; depth < DEFAULT_REORG_SAFETY_DEPTH - 1; ++depth)
        chain_setup->CreateAndProcessBlock({}, CScript() << OP_TRUE);
    update_tip();
    BOOST_CHECK(!admission());
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_HISTORY_FULL");
    const auto maintain = [&] {
        size_t expired{0};
        return history_store.ExpireAlternativeRecoveries(now, expired, error);
    };
    const auto active_recoveries = [&] {
        std::vector<AlternativeRecoveryRecord> active;
        BOOST_REQUIRE(WalletBatch{history_database}.ListPaymasterAlternativeRecoveries(active));
        return active.size();
    };
    BOOST_REQUIRE_MESSAGE(maintain(), error);
    BOOST_CHECK_EQUAL(active_recoveries(), 1U);

    // A real tip disconnect before the protection depth cannot archive the
    // exact signed record or release history capacity.
    auto& chainstate = node.chainman->ActiveChainstate();
    CBlockIndex* old_tip = WITH_LOCK(cs_main, return chainstate.m_chain.Tip());
    BlockValidationState disconnected;
    BOOST_REQUIRE_MESSAGE(chainstate.InvalidateBlock(disconnected, old_tip), disconnected.ToString());
    SyncWithValidationInterfaceQueue();
    update_tip();
    BOOST_REQUIRE_MESSAGE(maintain(), error);
    BOOST_CHECK_EQUAL(active_recoveries(), 1U);
    BOOST_CHECK(!admission());
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_HISTORY_FULL");
    WITH_LOCK(cs_main, chainstate.ResetBlockFailureFlags(old_tip));
    BlockValidationState reconnected;
    BOOST_REQUIRE_MESSAGE(chainstate.ActivateBestChain(reconnected), reconnected.ToString());
    SyncWithValidationInterfaceQueue();
    update_tip();
    chain_setup->CreateAndProcessBlock({}, CScript() << OP_TRUE);
    update_tip();
    // Delay this wallet's reorg observation: its cache still reports depth240
    // while canonical chain preflight sees239. Maintenance must retain it.
    CBlockIndex* stale_tip = WITH_LOCK(cs_main, return chainstate.m_chain.Tip());
    BlockValidationState stale_disconnect;
    BOOST_REQUIRE_MESSAGE(chainstate.InvalidateBlock(stale_disconnect, stale_tip), stale_disconnect.ToString());
    SyncWithValidationInterfaceQueue();
    IndexWaitSynced(*g_txindex);
    BOOST_REQUIRE_MESSAGE(maintain(), error);
    BOOST_CHECK_EQUAL(active_recoveries(), 1U);
    BOOST_CHECK(!admission());
    WITH_LOCK(cs_main, chainstate.ResetBlockFailureFlags(stale_tip));
    BlockValidationState stale_reconnect;
    BOOST_REQUIRE_MESSAGE(chainstate.ActivateBestChain(stale_reconnect), stale_reconnect.ToString());
    SyncWithValidationInterfaceQueue();
    update_tip();
    const auto before_archive = history_database.m_records;
    BOOST_CHECK(!admission()); // Read-only admission cannot perform maintenance.
    BOOST_CHECK(history_database.m_records == before_archive);

    history_database.m_pass = false;
    BOOST_CHECK(!maintain());
    history_database.m_pass = true;
    for (int scenario = 0; scenario < 4; ++scenario) {
        BOOST_TEST_CONTEXT("terminal evidence before archive scenario " << scenario) {
            history_database.m_records = before_archive;
            auto raw = history_database.MakeBatch();
            if (scenario == 0) {
                BOOST_REQUIRE(raw->Erase(std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT, commit.commit_key)));
            } else if (scenario == 1) {
                auto future = recovery;
                ++future.version;
                BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, recovery.recovery_id), future));
            } else if (scenario == 2) {
                auto corrupt = recovery;
                corrupt.recovery_response.identity_signature.back() ^= 1;
                BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, recovery.recovery_id), corrupt));
            } else {
                auto future = commit;
                ++future.version;
                BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT, commit.commit_key), future));
            }
            const auto damaged = history_database.m_records;
            BOOST_CHECK(!maintain());
            BOOST_CHECK(history_database.m_records == damaged);
        }
    }
    history_database.m_records = before_archive;
    history_database.FailWriteAt(0);
    BOOST_CHECK(!maintain());
    history_database.ClearFailureInjection();
    BOOST_CHECK(history_database.m_records == before_archive);
    AlignSecurityFixtureCheckpoint(history_database);
    history_database.FailCommit();
    BOOST_CHECK(!maintain());
    history_database.ClearFailureInjection();
    BOOST_CHECK(history_database.m_records == before_archive);
    AlignSecurityFixtureCheckpoint(history_database);
    BOOST_REQUIRE_MESSAGE(maintain(), error);
    BOOST_CHECK_EQUAL(active_recoveries(), 0U);
    BOOST_REQUIRE_MESSAGE(admission(), error);

    // Archive movement does not change accounting or exact replay authority.
    // Both lookups resolve the complete signed record without a history scan.
    const auto archived_records = history_database.m_records;
    BOOST_REQUIRE_MESSAGE(history_store.CommitProviderAlternativeRecoveryFinal(
        recovery, commit, result, result.genesis_hash, error), error);
    BOOST_CHECK(history_database.m_records == archived_records);
    AlternativeRecoveryRecord retained;
    BOOST_REQUIRE(history_store.GetAlternativeRecoveryById(recovery.recovery_id, retained));
    BOOST_CHECK(SerializePaymasterSecurityObject(retained) == SerializePaymasterSecurityObject(recovery));
    BOOST_REQUIRE_MESSAGE(history_store.GetProviderAlternativeRecoveryByCommit(commit, retained, error), error);
    BOOST_CHECK(SerializePaymasterSecurityObject(retained) == SerializePaymasterSecurityObject(recovery));
    ProviderFinanceEvent finance_before;
    BOOST_REQUIRE(WalletBatch{history_database}.ReadPaymasterFinanceEvent(commit.commit_key, finance_before) == DatabaseReadStatus::FOUND);
    size_t changed_events{0};
    BOOST_REQUIRE_MESSAGE(RebuildProviderFinances(history_wallet, changed_events, error), error);
    ProviderFinanceEvent finance_after;
    BOOST_REQUIRE(WalletBatch{history_database}.ReadPaymasterFinanceEvent(commit.commit_key, finance_after) == DatabaseReadStatus::FOUND);
    BOOST_CHECK(finance_after.kind == ProviderFinanceEventKind::RECOVERY);
    BOOST_CHECK(finance_after.dd_income == finance_before.dd_income);
    BOOST_CHECK(finance_after.dgb_cost == finance_before.dgb_cost);
    BOOST_CHECK(finance_after.transaction_id == finance_before.transaction_id);
    bool has_drain_work{true};
    BOOST_REQUIRE_MESSAGE(history_store.HasProviderDrainWork(identity.provider_id, has_drain_work, error), error);
    BOOST_CHECK(has_drain_work); // The new quote's live capacity continuation.

    {
        // Recreate a compatible historical replay cache for this genuine signed
        // and archived recovery. Isolate unrelated live-capacity rows so the
        // assertion concerns cache compaction rather than nonce expiry.
        wallet::CWallet cache_wallet{node.chain.get(), "archived-recovery-capacity-cache",
            DuplicateSecurityFixtureDatabase(history_database)};
        BOOST_REQUIRE(cache_wallet.LoadWallet() == DBErrors::LOAD_OK);
        const auto cache_tip = WITH_LOCK(history_wallet.cs_wallet,
            return std::make_pair(history_wallet.GetLastBlockHeight(), history_wallet.GetLastBlockHash()));
        {
            LOCK(cache_wallet.cs_wallet);
            cache_wallet.SetLastBlockProcessed(cache_tip.first, cache_tip.second);
        }
        WalletBatch cache_batch{cache_wallet.GetDatabase()};
        BOOST_REQUIRE(cache_batch.TxnBegin());
        for (const auto& cached : environment.capacity_records) {
            BOOST_REQUIRE(cache_batch.ErasePaymasterCapacityResponse(cached.request_hash));
            BOOST_REQUIRE(cache_batch.ErasePaymasterCapacityNonce(cached.client_nonce));
            BOOST_REQUIRE(cache_batch.ErasePaymasterCapacitySession(cached.session_key));
        }
        PaymasterCapacityProof proof;
        CDataStream encoded{recovery.capacity_snapshot.capacity_proof, SER_NETWORK, ::PROTOCOL_VERSION};
        encoded >> proof;
        BOOST_REQUIRE(encoded.empty());
        const auto request_hash = Hash(SerializePaymasterSecurityObject(recovery.capacity_request));
        BOOST_REQUIRE(cache_batch.WritePaymasterCapacityResponse(request_hash, recovery.capacity_snapshot.capacity_proof));
        BOOST_REQUIRE(cache_batch.WritePaymasterCapacityNonce(proof.client_nonce, request_hash));
        BOOST_REQUIRE(cache_batch.WritePaymasterCapacitySession(SecurityCapacitySessionKey(proof), request_hash));
        BOOST_REQUIRE(cache_batch.TxnCommit());
        ProviderBudgetLedger ledger_before;
        std::vector<ProviderPoolEntry> pool_before;
        BOOST_REQUIRE(ReadProviderSecurityState(cache_wallet, ledger_before, pool_before));
        PaymasterStore cache_store{cache_wallet};
        size_t expired{0};
        const int64_t cache_expiry = proof.expires_at + DEFAULT_RETRY_SECONDS + MAX_QUOTE_TTL_SECONDS + 1;
        BOOST_REQUIRE_MESSAGE(cache_store.ExpireProviderCapacityReservations(cache_expiry, expired, error), error);
        BOOST_CHECK_EQUAL(expired, 0U);
        ProviderBudgetLedger ledger_after;
        std::vector<ProviderPoolEntry> pool_after;
        BOOST_REQUIRE(ReadProviderSecurityState(cache_wallet, ledger_after, pool_after));
        BOOST_CHECK(SerializePaymasterSecurityObject(ledger_before) == SerializePaymasterSecurityObject(ledger_after));
        BOOST_CHECK(SerializePaymasterSecurityObject(pool_before) == SerializePaymasterSecurityObject(pool_after));
        AlternativeRecoveryRecord retained_after;
        BOOST_REQUIRE(cache_store.GetAlternativeRecoveryById(recovery.recovery_id, retained_after));
        BOOST_CHECK(SerializePaymasterSecurityObject(retained_after) == SerializePaymasterSecurityObject(recovery));
        std::vector<unsigned char> absent;
        BOOST_CHECK(cache_batch.ReadPaymasterCapacityResponseWithStatus(request_hash, absent) == DatabaseReadStatus::NOT_FOUND);
        const auto compacted = GetMockableDatabase(cache_wallet).m_records;
        BOOST_REQUIRE_MESSAGE(cache_store.CommitProviderAlternativeRecoveryFinal(
            recovery, commit, result, result.genesis_hash, error), error);
        BOOST_CHECK(GetMockableDatabase(cache_wallet).m_records == compacted);
    }
    AlignSecurityFixtureCheckpoint(history_database);
    const auto intact = history_database.m_records;
    for (int scenario = 0; scenario < 6; ++scenario) {
        BOOST_TEST_CONTEXT("archived recovery evidence scenario " << scenario) {
            history_database.m_records = intact;
            auto raw = history_database.MakeBatch();
            if (scenario == 0) {
                BOOST_REQUIRE(raw->Erase(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_ARCHIVE, recovery.recovery_id)));
            } else if (scenario == 1) {
                auto future = recovery;
                ++future.version;
                BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_ARCHIVE, recovery.recovery_id), future));
            } else if (scenario == 2) {
                auto corrupt = recovery;
                corrupt.recovery_response.identity_signature.back() ^= 1;
                BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_ARCHIVE, recovery.recovery_id), corrupt));
            } else if (scenario == 3) {
                BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, recovery.recovery_id), recovery));
            } else if (scenario == 4) {
                BOOST_REQUIRE(raw->Erase(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_LOCATOR, recovery.recovery_id)));
            } else {
                BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_LOCATOR, recovery.recovery_id), uint256::ONE));
            }
            const auto damaged = history_database.m_records;
            BOOST_CHECK(!history_store.GetProviderAlternativeRecoveryByCommit(commit, retained, error));
            BOOST_CHECK(!history_store.CommitProviderAlternativeRecoveryFinal(
                recovery, commit, result, result.genesis_hash, error));
            BOOST_CHECK(!RebuildProviderFinances(history_wallet, changed_events, error));
            BOOST_CHECK(history_database.m_records == damaged);
        }
    }
    history_database.m_records = intact;
    BOOST_REQUIRE_MESSAGE(admission(), error);
    wallet::CWallet terminal_reopened{node.chain.get(), "completed-recovery-history-reopened",
        DuplicateSecurityFixtureDatabase(history_database)};
    BOOST_REQUIRE(terminal_reopened.LoadWallet() == DBErrors::LOAD_OK);
    const auto terminal_tip = WITH_LOCK(history_wallet.cs_wallet,
        return std::make_pair(history_wallet.GetLastBlockHeight(), history_wallet.GetLastBlockHash()));
    {
        LOCK(terminal_reopened.cs_wallet);
        terminal_reopened.SetLastBlockProcessed(terminal_tip.first, terminal_tip.second);
        WalletBatch batch{terminal_reopened.GetDatabase()};
        BOOST_CHECK(paymaster_store::internal::CheckProviderHistoryCapacity(batch,
            next_quote.request.intent.request_id, error));
    }
}

static void ExerciseClientRecoveryResult(ClientRecoveryReadSetup& setup, bool confirmed_before_result,
                                         bool exercise_provider_history = false,
                                         bool exercise_quote_intake = false,
                                         bool observe_without_result = false,
                                         bool permanent_provider_loss = false)
{
    auto& m_node = setup.m_node;
    auto& context = setup.context;
    auto& manager = setup.manager;
    auto& fixture = setup.fixture;
    auto& environment = setup.environment;
    auto& now = setup.now;
    const auto& seed = setup.seed;
    const auto& m_coinbase_txns = setup.m_coinbase_txns;
    const auto& coinbaseKey = setup.coinbaseKey;
    // Reuse the real readiness/index/transport fixture. The recovery inputs
    // below are mined and its final transaction passes ordinary mempool
    // validation; no unchecked mempool insertion or synthetic known-final path.
    wallet::CWallet client_wallet{m_node.chain.get(), "recovery-result-client", CreateMockableWalletDatabase()};
    BOOST_REQUIRE(client_wallet.LoadWallet() == DBErrors::LOAD_OK);
    wallet::CWallet& client = client_wallet;
    struct OracleCleanup {
        ~OracleCleanup()
        {
            OracleBundleManager::GetInstance().Clear();
            MockOracleManager::GetInstance().Reset();
        }
    } oracle_cleanup;
    OracleBundleManager::GetInstance().Clear();
    MockOracleManager::GetInstance().Reset();
    CKey user_key, return_key, provider_key;
    std::string error;
    const auto script_for = [&](CKey& key) {
        key.MakeNewKey(true);
        BOOST_REQUIRE_MESSAGE(AddTaprootSigningKey(client, key, error), error);
        TaprootBuilder builder;
        builder.Finalize(XOnlyPubKey{key.GetPubKey()});
        return GetScriptForDestination(builder.GetOutput());
    };
    script_for(user_key);
    const CScript return_script = script_for(return_key);
    const CScript provider_script = script_for(provider_key);
    constexpr CAmount principal{10000}, collateral{500 * COIN}, network_fee{COIN / 10};
    std::vector<CMutableTransaction> funding_transactions{
        setup.CreateValidMempoolTransaction(m_coinbase_txns[0], 0, 1, coinbaseKey, CScript() << OP_TRUE, collateral + COIN, false),
        setup.CreateValidMempoolTransaction(m_coinbase_txns[1], 0, 2, coinbaseKey, provider_script, 2 * COIN, false)};
    if (confirmed_before_result) {
        funding_transactions.push_back(setup.CreateValidMempoolTransaction(m_coinbase_txns[2], 0, 3,
            coinbaseKey, environment.identity.identity_script, 2 * COIN, false));
    }
    const CBlock funding = setup.CreateAndProcessBlock(funding_transactions, CScript() << OP_TRUE);
    BOOST_REQUIRE(funding.GetHash() == WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()));
    const int mint_height = *m_node.chain->getHeight() + 1;
    auto& mock_oracle = MockOracleManager::GetInstance();
    mock_oracle.SetMockPrice(1000000);
    const auto bundle = mock_oracle.CreateMockMuSig2Bundle(mint_height);
    auto& oracle = OracleBundleManager::GetInstance();
    oracle.SetEnabled(true);
    BOOST_REQUIRE_MESSAGE(OracleBundleManager::ValidateMuSig2Bundle(bundle, mint_height, Params().GetConsensus(), error), error);
    BOOST_REQUIRE(oracle.UpdateBundle(bundle));
    DigiDollar::MintParams mint_parameters;
    mint_parameters.ddAmount = principal;
    mint_parameters.lockHeight = mint_height + DigiDollar::LockDaysToBlocks(30);
    mint_parameters.ownerKey = XOnlyPubKey{user_key.GetPubKey()};
    mint_parameters.internalKey = DigiDollar::GetCollateralNUMSKey();
    CMutableTransaction mint;
    mint.SetDigiDollarType(DD_TX_MINT);
    mint.vin.emplace_back(COutPoint{funding.vtx.at(1)->GetHash(), 0});
    mint.vout.emplace_back(collateral, DigiDollar::CreateCollateralP2TR(mint_parameters));
    mint.vout.emplace_back(0, DigiDollar::CreateDigiDollarP2TR(mint_parameters.ownerKey, principal));
    mint.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
        << CScriptNum(1) << CScriptNum(principal) << CScriptNum(mint_parameters.lockHeight) << CScriptNum(1)
        << std::vector<unsigned char>(mint_parameters.ownerKey.begin(), mint_parameters.ownerKey.end()));
    const CBlock minted = setup.CreateAndProcessBlock({mint}, CScript() << OP_TRUE);
    BOOST_REQUIRE(minted.GetHash() == WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()));
    IndexWaitSynced(*g_txindex);
    {
        LOCK(client.cs_wallet);
        BOOST_REQUIRE(client.AddToWallet(funding.vtx.at(2), TxStateConfirmed{funding.GetHash(), mint_height - 1, 2}));
        BOOST_REQUIRE(client.AddToWallet(minted.vtx.at(1), TxStateConfirmed{minted.GetHash(), mint_height, 1}));
        client.SetLastBlockProcessed(mint_height, minted.GetHash());
    }
    now = GetTime();
    BOOST_REQUIRE_MESSAGE(paymaster_app::CheckPaymasterClientReadiness(client, context, error), error);
    BOOST_REQUIRE_MESSAGE(paymaster_app::CheckPaymasterPrivacyReadiness(PrivacyProfile::STANDARD, context, error), error);
    PaymasterStore store{client};
    PaymentSession session;
    BOOST_REQUIRE(store.CreateOrJoinSession("550e8400-e29b-41d4-a716-446655441097",
        SecurityTestId(seed, 120), FeeMode::PAYMASTER, now, session, error) == CreatePaymasterSessionResult::CREATED);
    session.state = SessionState::PENDING_PROVIDER;
    session.user_inputs = {COutPoint{minted.vtx.at(1)->GetHash(), 1}};
    ProviderAttempt original_attempt{fixture.attempt};
    if (confirmed_before_result) {
        // Native observations deliberately require a retained original attempt:
        // a tombstone's txid alone is never a receipt. Keep the actual original
        // USER_SIGNED authority on these mined inputs, independently of the
        // alternative provider's capacity, signatures, and final transaction.
        ProviderQuoteFixture original;
        original.user_key = user_key;
        original.user_transaction = minted.vtx.at(1);
        ProviderPolicy original_policy{environment.policy};
        original_policy.maximum_network_fee = DGBSatoshis{network_fee};
        PaymentIntent& intent = original.request.intent;
        intent.genesis_hash = Params().GenesisBlock().GetHash();
        intent.provider_id = environment.identity.provider_id;
        intent.request_id = session.request_id;
        intent.session_id = session.session_id;
        intent.canonical_request_hash = session.canonical_request_hash;
        intent.client_nonce = SecurityTestId(seed, 140);
        intent.user_dd_inputs = session.user_inputs;
        intent.recipient_script = NewTaprootScript();
        intent.recipient_amount = DDCents{1000};
        intent.user_dd_change_script = return_script;
        intent.offer_id = SecurityTestId(seed, 141);
        intent.funding_model = FundingModel::SPONSORED;
        intent.sponsorship_scope = SponsorshipScope::PUBLIC;
        intent.policy_hash = GetProviderPolicyHash(original_policy);
        intent.expires_at = now + 60;
        std::vector<XOnlyPubKey> output_keys;
        BOOST_REQUIRE_MESSAGE(SignPaymentIntentInputs(client, intent, now, output_keys, error), error);
        ProviderQuoteBuildParameters build;
        build.quote_id = SecurityTestId(seed, 142);
        build.user_inputs = {{session.user_inputs.front(), original.user_transaction, InputRole::USER_DD}};
        build.reserved_dgb_inputs = {{COutPoint{funding.vtx.at(3)->GetHash(), 0},
            CMutableTransaction{*funding.vtx.at(3)}, DGBSatoshis{2 * COIN}}};
        build.dgb_change_script = environment.identity.identity_script;
        build.network_fee = DGBSatoshis{network_fee};
        build.created_at = now;
        build.expires_at = now + 30;
        build.retry_until = now + 300;
        ProviderQuoteBuildResult built;
        {
            LOCK(cs_main);
            BOOST_REQUIRE_MESSAGE(BuildUnsignedProviderQuote(original.request, original_policy, build,
                Params(), m_node.chainman->ActiveChainstate().CoinsTip(), built, error), error);
        }
        original.response.request_id = session.request_id;
        original.response.session_id = session.session_id;
        original.response.quote = built.quote;
        auto& quote = original.response.quote;
        quote.identity_signature.resize(64);
        BOOST_REQUIRE(environment.identity_key.SignSchnorr(GetPaymasterQuoteSignatureHash(quote),
            quote.identity_signature, nullptr, uint256{}));
        auto& attempt = original.attempt;
        attempt.session_id = session.session_id;
        attempt.attempt_id = SecurityTestId(seed, 143);
        attempt.provider_id = intent.provider_id;
        attempt.provider_identity_key = environment.identity.identity_key;
        attempt.provider_endpoint = "127.0.0.1:18444";
        attempt.client_nonce = intent.client_nonce;
        attempt.intent_hash = quote.intent_hash;
        attempt.quote_id = quote.quote_id;
        attempt.unsigned_txid = quote.unsigned_txid;
        attempt.template_commitment = quote.template_commitment;
        attempt.commit_key = GetPaymasterCommitKey(attempt.provider_id, attempt.client_nonce,
            attempt.intent_hash, attempt.quote_id, attempt.template_commitment);
        PaymentIntent unsigned_intent{intent};
        unsigned_intent.user_input_proofs.clear();
        attempt.unsigned_intent = SerializePaymasterSecurityObject(unsigned_intent);
        attempt.quote_request = SerializePaymasterSecurityObject(original.request);
        attempt.signed_quote = SerializePaymasterSecurityObject(original.response);
        attempt.unsigned_transaction = SerializePaymasterSecurityObject(quote.unsigned_transaction);
        attempt.unsigned_psbt = SerializePaymasterSecurityObject(built.trusted_template.psbt);
        attempt.input_roles = {ReservationRole::USER_DD, ReservationRole::PROVIDER_DGB};
        attempt.quote_expires_at = quote.expires_at;
        attempt.retry_until = quote.retry_until;
        attempt.created_at = attempt.updated_at = now;
        PaymasterResult unused_result;
        CMutableTransaction unused_final;
        BOOST_REQUIRE_MESSAGE(BuildDurableClientFinalFixture(client, environment, original,
            now, session, unused_result, unused_final, error), error);
        attempt.state = AttemptState::USER_SIGNED;
        attempt.final_transaction.clear();
        attempt.final_txid.SetNull();
        attempt.updated_at = now;
        session.pending_phase = PendingPhase::USER_SIGNATURE_SENT;
        session.final_txid.SetNull();
        session.updated_at = now;
        original_attempt = attempt;
        // The signing helper also supports unconfirmed fixtures. Restore the
        // real confirmations for this chain-backed variant.
        LOCK(client.cs_wallet);
        BOOST_REQUIRE(client.AddToWallet(funding.vtx.at(3),
            TxStateConfirmed{funding.GetHash(), mint_height - 1, 3}));
        BOOST_REQUIRE(client.AddToWallet(minted.vtx.at(1),
            TxStateConfirmed{minted.GetHash(), mint_height, 1}));
    }
    // This test begins with saved recovery authority. The original binding is
    // independent of the complete recovery manifest and actual input witnesses.
    AlternativeRecoveryParameters parameters;
    parameters.genesis_hash = Params().GenesisBlock().GetHash();
    parameters.request_id = session.request_id;
    parameters.session_id = session.session_id;
    parameters.original_provider_id = original_attempt.provider_id;
    parameters.original_commit_key = original_attempt.commit_key;
    parameters.original_template_commitment = original_attempt.template_commitment;
    parameters.recovery_provider_identity_key = XOnlyPubKey{provider_key.GetPubKey()};
    parameters.recovery_provider_id = GetPaymasterId(parameters.recovery_provider_identity_key);
    parameters.offer_id = SecurityTestId(seed, 121);
    parameters.policy_hash = SecurityTestId(seed, 122);
    auto& capacity_request = parameters.capacity_request;
    capacity_request.genesis_hash = parameters.genesis_hash;
    capacity_request.provider_id = parameters.recovery_provider_id;
    capacity_request.request_id = session.request_id;
    capacity_request.session_id = session.session_id;
    capacity_request.client_nonce = SecurityTestId(seed, 123);
    capacity_request.funding_model = FundingModel::USER_PAID;
    capacity_request.created_at = now;
    capacity_request.expires_at = now + 60;
    PaymasterCapacityProof proof;
    proof.genesis_hash = parameters.genesis_hash;
    proof.provider_id = capacity_request.provider_id;
    proof.request_id = session.request_id;
    proof.session_id = session.session_id;
    proof.client_nonce = capacity_request.client_nonce;
    proof.funding_model = FundingModel::USER_PAID;
    proof.snapshot_id = SecurityTestId(seed, 124);
    proof.created_at = now;
    proof.expires_at = now + 60;
    CapacityDGBInput dgb;
    dgb.input = {COutPoint{funding.vtx.at(2)->GetHash(), 0}, CMutableTransaction{*funding.vtx.at(2)}, DGBSatoshis{2 * COIN}};
    dgb.control_proof.reference_block = minted.GetHash();
    dgb.control_proof.expires_at = proof.expires_at;
    dgb.control_proof.signature.resize(64);
    const uint256 empty_merkle_root;
    BOOST_REQUIRE(provider_key.SignSchnorr(GetCapacityControlHash(proof, dgb.input.outpoint, proof.expires_at),
        dgb.control_proof.signature, &empty_merkle_root, uint256{}));
    PaymasterLiquiditySlot slot;
    slot.dgb_inputs = {dgb};
    proof.liquidity_slots = {slot};
    proof.identity_signature.resize(64);
    BOOST_REQUIRE(provider_key.SignSchnorr(GetCapacityProofSignatureHash(proof), proof.identity_signature, nullptr, uint256{}));
    auto& snapshot = parameters.capacity_snapshot;
    snapshot.snapshot_id = proof.snapshot_id;
    snapshot.resource_commitment = GetCapacityResourceCommitment(proof);
    snapshot.session_id = session.session_id;
    snapshot.attempt_id = SecurityTestId(seed, 125);
    snapshot.provider_id = proof.provider_id;
    snapshot.client_nonce = proof.client_nonce;
    snapshot.request_hash = Hash(SerializePaymasterSecurityObject(capacity_request));
    snapshot.capacity_proof = SerializePaymasterSecurityObject(proof);
    snapshot.created_at = snapshot.validated_at = now;
    snapshot.expires_at = proof.expires_at;
    snapshot.funding_model = FundingModel::USER_PAID;
    parameters.persisted_user_dd_inputs = session.user_inputs;
    parameters.user_dd_inputs = {{session.user_inputs.front(), minted.vtx.at(1), InputRole::USER_DD}};
    parameters.wallet_returns = {{return_script, DDCents{principal - 100}}};
    parameters.wallet_verified_fresh_return_scripts = {return_script};
    parameters.recovery_provider_dd_script = provider_script;
    parameters.recovery_provider_dgb_change_script = provider_script;
    parameters.maximum_service_fee = parameters.service_fee = DDCents{100};
    parameters.network_fee = DGBSatoshis{network_fee};
    parameters.fee_rate = 1;
    parameters.expires_at = now + 30;
    AlternativeRecoveryTemplate trusted;
    BOOST_REQUIRE_MESSAGE(BuildAlternativeRecoveryTemplateAgainstChainstate(parameters, Params(), *m_node.chainman, now, trusted, error), error);
    auto request = MakeRecoveryReadRequest(environment, seed, now);
    request.genesis_hash = parameters.genesis_hash;
    request.request_id = session.request_id;
    request.session_id = session.session_id;
    request.original_provider_id = parameters.original_provider_id;
    request.recovery_provider_id = parameters.recovery_provider_id;
    request.offer_id = parameters.offer_id;
    request.policy_hash = parameters.policy_hash;
    request.original_commit_key = parameters.original_commit_key;
    request.original_template_commitment = parameters.original_template_commitment;
    request.client_nonce = capacity_request.client_nonce;
    request.capacity_request = capacity_request;
    request.capacity_snapshot_id = snapshot.snapshot_id;
    request.capacity_resource_commitment = snapshot.resource_commitment;
    request.user_dd_inputs = session.user_inputs;
    request.wallet_returns = parameters.wallet_returns;
    request.created_at = now;
    request.expires_at = parameters.expires_at;
    request.user_input_proofs = {{session.user_inputs.front(), std::vector<unsigned char>(64)}};
    BOOST_REQUIRE(user_key.SignSchnorr(GetAlternativeRecoveryInputControlHash(request, session.user_inputs.front()),
        request.user_input_proofs.front().signature, &empty_merkle_root, uint256{}));
    AlternativeRecoveryRecord recovery;
    recovery.request_id = session.request_id;
    recovery.session_id = session.session_id;
    recovery.original_provider_id = parameters.original_provider_id;
    recovery.recovery_provider_id = parameters.recovery_provider_id;
    recovery.recovery_provider_identity_key = parameters.recovery_provider_identity_key;
    recovery.recovery_provider_endpoint = "127.0.0.1:18445";
    recovery.offer_id = parameters.offer_id;
    recovery.policy_hash = parameters.policy_hash;
    recovery.original_commit_key = parameters.original_commit_key;
    recovery.original_template_commitment = parameters.original_template_commitment;
    recovery.selected_maximum_service_fee = recovery.selected_service_fee = DDCents{100};
    recovery.client_nonce = request.client_nonce;
    recovery.capacity_request = capacity_request;
    recovery.capacity_snapshot = snapshot;
    recovery.recovery_request = request;
    recovery.recovery_id = GetAlternativeRecoveryId(session.request_id, session.session_id, recovery.recovery_provider_id, request.client_nonce);
    recovery.recovery_request_hash = GetAlternativeRecoveryRequestHash(request);
    recovery.phase = AlternativeRecoveryPhase::RESPONSE_VALIDATED;
    recovery.created_at = recovery.updated_at = now;
    auto& response = recovery.recovery_response;
    response.genesis_hash = parameters.genesis_hash;
    response.request_id = session.request_id;
    response.session_id = session.session_id;
    response.recovery_id = recovery.recovery_id;
    response.recovery_provider_id = recovery.recovery_provider_id;
    response.recovery_request_hash = recovery.recovery_request_hash;
    response.manifest = trusted.manifest;
    response.unsigned_psbt = SerializePaymasterSecurityObject(trusted.trusted_template.psbt);
    response.created_at = now;
    response.expires_at = parameters.expires_at;
    response.recovery_commit_key = GetAlternativeRecoveryCommitKey(response);
    response.identity_signature.resize(64);
    BOOST_REQUIRE(provider_key.SignSchnorr(GetAlternativeRecoveryResponseSignatureHash(response), response.identity_signature, nullptr, uint256{}));
    BOOST_REQUIRE_MESSAGE(BuildRecoveryAuthorizationManifest(request, response, recovery.recovery_authorization, error), error);
    ClientSafetyPolicy policy;
    policy.maximum_service_fee_per_transaction = policy.maximum_service_fee_per_day = DDCents{100};
    policy.updated_at = now;
    ClientFeeLedger ledger;
    ledger.accounting_time_high_water = now;
    if (permanent_provider_loss) {
        policy.maximum_service_fee_per_day = DDCents{200};
        BOOST_REQUIRE_MESSAGE(ReserveClientFee(ledger, policy, original_attempt.commit_key, DDCents{5}, now, error), error);
    }
    {
        LOCK(client.cs_wallet);
        WalletBatch batch{client.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterSession(session));
        if (confirmed_before_result) {
            BOOST_REQUIRE(batch.WritePaymasterAttempt(original_attempt));
            BOOST_REQUIRE(batch.WritePaymasterTemplate(original_attempt.template_commitment, original_attempt.attempt_id));
            BOOST_REQUIRE(batch.WritePaymasterUnsignedTx(original_attempt.unsigned_txid, original_attempt.attempt_id));
        }
        BOOST_REQUIRE(batch.WritePaymasterReservation({InputReservation::CURRENT_VERSION, session.user_inputs.front(), session.request_id,
            session.session_id, ReservationRole::USER_DD, true, now}));
        BOOST_REQUIRE(batch.WritePaymasterLockedUTXO(session.user_inputs.front()));
        client.LockPaymasterCoin(session.user_inputs.front());
        BOOST_REQUIRE(batch.WritePaymasterClientSafetyPolicy(policy));
        BOOST_REQUIRE(batch.WritePaymasterClientFeeLedger(ledger));
        BOOST_REQUIRE(batch.WritePaymasterAlternativeRecovery(recovery));
        BOOST_REQUIRE(batch.WritePaymasterAlternativeRecoveryRequest(session.request_id, recovery.recovery_id));
    }
    if (exercise_quote_intake) {
        const AlternativeRecoveryResponse reply{response};
        recovery.phase = AlternativeRecoveryPhase::REQUEST_READY;
        recovery.recovery_response = {};
        recovery.recovery_authorization = {};
        BOOST_REQUIRE_MESSAGE(paymaster_store::internal::ValidateAlternativeRecoveryRecordShape(recovery, error), error);
        WalletBatch batch{client.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterAlternativeRecovery(recovery));
        const AlternativeRecoveryRecord request_ready{recovery};
        const auto encoded = SerializePaymasterSecurityObject(reply);
        const uint256 message_id{Hash(encoded)};
        BOOST_REQUIRE(manager.EnqueueDirectMessage(45, message_id, encoded.size(), DirectPayload{reply}, now));
        paymaster_app::ClientRecoveryOptions options;
        options.prepare_only = true;
        const auto invoke = [&] {
            return paymaster_app::ResolveAlternativePaymasterRecovery(
                context, client, store, session, original_attempt, options);
        };
        auto& database = GetMockableDatabase(client);
        const auto unchanged_fee_ledger = SerializePaymasterSecurityObject(ledger);
        const auto check_unsigned_reservation = [&] {
            AlternativeRecoveryRecord saved;
            BOOST_REQUIRE(store.GetAlternativeRecovery(session.request_id, saved));
            BOOST_CHECK(saved.user_signed_psbt.empty());
            BOOST_CHECK(saved.final_transaction.empty());
            BOOST_CHECK(saved.accepted_recovery_authorization_commitment.IsNull());
            BOOST_CHECK_EQUAL(saved.recovery_authorization_accepted_at, 0);
            ClientFeeLedger retained;
            BOOST_REQUIRE(batch.ReadPaymasterClientFeeLedger(retained));
            BOOST_CHECK(SerializePaymasterSecurityObject(retained) == unchanged_fee_ledger);
            BOOST_CHECK(IsPaymasterInputReserved(client, session.user_inputs.front()));
            BOOST_CHECK(WITH_LOCK(client.cs_wallet, return client.IsLockedCoin(session.user_inputs.front())));
        };

        // A readable local defect must be found before interpreting or
        // discarding the selected provider's valid response.
        auto damaged = request_ready;
        damaged.recovery_request.original_commit_key = SecurityTestId(seed, 150);
        BOOST_REQUIRE(batch.WritePaymasterAlternativeRecovery(damaged));
        const auto damaged_rows = database.m_records;
        BOOST_CHECK_EXCEPTION(invoke(), ApplicationError,
            [](const ApplicationError& failure) {
                return failure.kind == ErrorKind::WALLET &&
                       failure.code == "PAYMASTER_RECOVERY_DATABASE_READ";
            });
        BOOST_CHECK(database.m_records == damaged_rows);
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
        check_unsigned_reservation();
        BOOST_REQUIRE(batch.WritePaymasterAlternativeRecovery(request_ready));

        // With intact local authority a forged peer signature is terminal,
        // but neither a recovery authorization nor a user signature is saved.
        {
            Manager invalid_manager{true};
            context.paymaster = &invalid_manager;
            auto invalid_reply = reply;
            invalid_reply.identity_signature.back() ^= 1;
            const auto invalid_bytes = SerializePaymasterSecurityObject(invalid_reply);
            BOOST_REQUIRE(invalid_manager.EnqueueDirectMessage(45, Hash(invalid_bytes),
                invalid_bytes.size(), DirectPayload{invalid_reply}, now));
            const auto before = database.m_records;
            BOOST_CHECK_EXCEPTION(invoke(), ApplicationError,
                [](const ApplicationError& failure) {
                    return failure.kind == ErrorKind::INVALID_INPUT &&
                           failure.code == "PAYMASTER_RECOVERY_RESPONSE_MISMATCH";
                });
            BOOST_CHECK_EQUAL(invalid_manager.DirectMessageCount(), 0U);
            BOOST_CHECK(database.m_records == before);
            check_unsigned_reservation();
            context.paymaster = &manager;
        }

        for (const bool fail_commit : {false, true}) {
            const auto before = database.m_records;
            if (fail_commit) database.FailCommit();
            else database.FailWriteAt(0);
            BOOST_CHECK_EXCEPTION(invoke(), ApplicationError,
                [](const ApplicationError& failure) {
                    return failure.kind == ErrorKind::WALLET &&
                           failure.code == "PAYMASTER_DATABASE_WRITE";
                });
            database.ClearFailureInjection();
            BOOST_CHECK(database.m_records == before);
            BOOST_REQUIRE(store.GetAlternativeRecovery(session.request_id, recovery));
            BOOST_CHECK(recovery.phase == AlternativeRecoveryPhase::REQUEST_READY);
            BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
            check_unsigned_reservation();
        }

        const auto prepared = invoke();
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
        BOOST_REQUIRE(store.GetAlternativeRecovery(session.request_id, recovery));
        BOOST_CHECK(recovery.phase == AlternativeRecoveryPhase::RESPONSE_VALIDATED);
        BOOST_CHECK(SerializePaymasterSecurityObject(recovery.recovery_response) == encoded);
        BOOST_CHECK(!recovery.recovery_authorization.authorization_commitment.IsNull());
        BOOST_REQUIRE(prepared.view.recovery);
        BOOST_CHECK(prepared.view.recovery->phase == AlternativeRecoveryPhase::RESPONSE_VALIDATED);
        check_unsigned_reservation();
        const auto committed = database.m_records;
        BOOST_CHECK_NO_THROW(invoke());
        BOOST_CHECK(database.m_records == committed);
        return;
    }
    BOOST_REQUIRE_MESSAGE(store.AcceptAlternativeRecoveryAuthorization(session.request_id,
        recovery.recovery_authorization.authorization_commitment, now, error), error);
    if (confirmed_before_result) {
        BOOST_REQUIRE_MESSAGE(paymaster_store::internal::ValidateAttemptAuthorizationManifest(
            original_attempt, /*provider_side=*/false, error), error);
    }
    BOOST_REQUIRE(store.GetAlternativeRecovery(session.request_id, recovery));
    auto signed_psbt = trusted.trusted_template.psbt;
    BOOST_REQUIRE_MESSAGE(SignCollaborativePSBTForParty(client, signed_psbt, trusted.trusted_template, SigningParty::USER, error), error);
    recovery.user_signed_psbt = SerializePaymasterSecurityObject(signed_psbt);
    recovery.phase = AlternativeRecoveryPhase::USER_SIGNED;
    BOOST_REQUIRE_MESSAGE(store.UpdateAlternativeRecovery(recovery, error), error);
    BOOST_REQUIRE_MESSAGE(SignCollaborativePSBTForParty(client, signed_psbt, trusted.trusted_template, SigningParty::PROVIDER, error), error);
    CMutableTransaction final_transaction;
    BOOST_REQUIRE(FinalizeAndExtractPSBT(signed_psbt, final_transaction));
    AlternativeRecoveryResultMessage result;
    result.request_id = session.request_id;
    result.session_id = session.session_id;
    result.recovery_id = recovery.recovery_id;
    result.recovery_request_hash = recovery.recovery_request_hash;
    result.result.genesis_hash = parameters.genesis_hash;
    result.result.provider_id = recovery.recovery_provider_id;
    result.result.commit_key = response.recovery_commit_key;
    result.result.result_sequence = 1;
    result.result.status = PaymasterResultStatus::FINAL_COMMITTED;
    result.result.txid = CTransaction{final_transaction}.GetHash();
    result.result.raw_transaction_hash = CTransaction{final_transaction}.GetWitnessHash();
    result.result.final_transaction = final_transaction;
    result.result.updated_at = now;
    result.result.identity_signature.resize(64);
    BOOST_REQUIRE(provider_key.SignSchnorr(GetPaymasterResultSignatureHash(result.result), result.result.identity_signature, nullptr, uint256{}));
    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoveryResult(result, response, recovery.recovery_provider_identity_key,
        parameters.genesis_hash, 1, now, error), error);
    if (!confirmed_before_result) {
        ExerciseFirstProviderRecoveryCommit(m_node, recovery, provider_key, funding.vtx.at(2), result.result, now);
    }
    if (confirmed_before_result) {
        const CBlock confirmed = setup.CreateAndProcessBlock({final_transaction}, CScript() << OP_TRUE);
        BOOST_REQUIRE(confirmed.GetHash() == WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()));
        IndexWaitSynced(*g_txindex);
        const int confirmed_height = *m_node.chain->getHeight();
        {
            LOCK(client.cs_wallet);
            BOOST_REQUIRE(client.AddToWallet(confirmed.vtx.at(1),
                TxStateConfirmed{confirmed.GetHash(), confirmed_height, 1}));
            client.SetLastBlockProcessed(confirmed_height, confirmed.GetHash());
        }
        if (exercise_provider_history) {
            const TxStateConfirmed confirmed_state{confirmed.GetHash(), confirmed_height, 1};
            ExerciseFirstProviderRecoveryCommit(m_node, recovery, provider_key, funding.vtx.at(2),
                result.result, now, &setup, &confirmed_state);
            return;
        }
        {
            LOCK(cs_main);
            for (const auto& input : final_transaction.vin) {
                BOOST_CHECK(m_node.chainman->ActiveChainstate().CoinsTip().AccessCoin(input.prevout).IsSpent());
            }
        }
        SetMockTime(now + MAX_DIRECT_MESSAGE_TTL_SECONDS + 1);
    }
    if (observe_without_result && !confirmed_before_result) {
        bool already_confirmed{false};
        BOOST_REQUIRE_MESSAGE(paymaster_app::InsertAndBroadcastPaymasterTransaction(client,
            MakeTransactionRef(final_transaction), already_confirmed, error), error);
        BOOST_CHECK(!already_confirmed);
        BOOST_REQUIRE(m_node.mempool->get(CTransaction{final_transaction}.GetHash()));
    }
    if (confirmed_before_result || observe_without_result) {
        // No provider reply exists yet, and all inputs are spent. The exact
        // local final must allow the same USER_SIGNED recovery to resume even
        // after expiry; it must not manufacture a result or consume fees.
        const auto before_retry = GetMockableDatabase(client).m_records;
        const auto retry_without_result = paymaster_app::ResolveAlternativePaymasterRecovery(
            context, client, store, session, original_attempt, {});
        BOOST_CHECK(!retry_without_result.broadcast.value_or(false));
        BOOST_CHECK(GetMockableDatabase(client).m_records == before_retry);
        BOOST_REQUIRE(store.GetAlternativeRecovery(session.request_id, recovery));
        BOOST_CHECK(recovery.phase == AlternativeRecoveryPhase::USER_SIGNED);
        BOOST_CHECK_EQUAL(recovery.signed_result.result_sequence, 0U);
        const auto final_txid = CTransaction{final_transaction}.GetHash();
        CTransactionRef intact;
        {
            LOCK(client.cs_wallet);
            auto& observed = client.mapWallet.at(final_txid);
            intact = observed.tx;
            auto altered = CMutableTransaction{*intact};
            BOOST_REQUIRE(!altered.vin.front().scriptWitness.stack.empty());
            altered.vin.front().scriptWitness.stack.front().back() ^= 1;
            observed.tx = MakeTransactionRef(altered);
        }
        BOOST_CHECK_THROW(paymaster_app::ResolveAlternativePaymasterRecovery(
            context, client, store, session, original_attempt, {}), ApplicationError);
        WITH_LOCK(client.cs_wallet, client.mapWallet.at(final_txid).tx = intact);
        BOOST_CHECK(GetMockableDatabase(client).m_records == before_retry);
    }
    if (permanent_provider_loss) {
        BOOST_REQUIRE(confirmed_before_result);
        const auto reserved_input = session.user_inputs.front();
        const auto recovery_fee_key = response.recovery_commit_key;
        bool completed{false};
        BOOST_REQUIRE_MESSAGE(store.CompleteClientConfirmedFinal(session.request_id, GetTime(), completed, error), error);
        BOOST_CHECK(!completed);
        for (int depth = 1; depth < DEFAULT_REORG_SAFETY_DEPTH - 1; ++depth)
            setup.CreateAndProcessBlock({}, CScript() << OP_TRUE);
        const auto update_client_tip = [&] {
            IndexWaitSynced(*g_txindex);
            const auto tip = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
            LOCK(client.cs_wallet);
            client.SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
        };
        update_client_tip();
        BOOST_REQUIRE_MESSAGE(store.CompleteClientConfirmedFinal(session.request_id, GetTime(), completed, error), error);
        BOOST_CHECK(!completed); // 239 confirmations still retain every artifact.
        auto& chainstate = m_node.chainman->ActiveChainstate();
        auto* tip_before_reorg = WITH_LOCK(cs_main, return chainstate.m_chain.Tip());
        BlockValidationState disconnected;
        BOOST_REQUIRE_MESSAGE(chainstate.InvalidateBlock(disconnected, tip_before_reorg), disconnected.ToString());
        update_client_tip();
        BOOST_REQUIRE_MESSAGE(store.CompleteClientConfirmedFinal(session.request_id, GetTime(), completed, error), error);
        BOOST_CHECK(!completed);
        for (int block = 0; block < 2; ++block) setup.CreateAndProcessBlock({}, CScript() << OP_2);
        IndexWaitSynced(*g_txindex);
        const int observed_height = *m_node.chain->getHeight();
        const auto observed_hash = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        { LOCK(client.cs_wallet); client.SetLastBlockProcessed(observed_height, observed_hash); }
        auto& failed_database = GetMockableDatabase(client);
        const auto unchanged = failed_database.m_records;
        failed_database.FailCommit();
        BOOST_CHECK(!store.CompleteClientConfirmedFinal(session.request_id, GetTime(), completed, error));
        failed_database.ClearFailureInjection();
        BOOST_CHECK(failed_database.m_records == unchanged);
        BOOST_CHECK(IsPaymasterInputReserved(client, session.user_inputs.front()));
        CTransactionRef exact_final;
        {
            LOCK(client.cs_wallet);
            auto& observed = client.mapWallet.at(final_transaction.GetHash());
            exact_final = observed.tx;
            CMutableTransaction altered{*exact_final};
            altered.vin.front().scriptWitness.stack.front().back() ^= 1;
            observed.tx = MakeTransactionRef(altered);
        }
        BOOST_CHECK(!store.CompleteClientConfirmedFinal(session.request_id, GetTime(), completed, error));
        BOOST_CHECK(failed_database.m_records == unchanged);
        WITH_LOCK(client.cs_wallet, client.mapWallet.at(final_transaction.GetHash()).tx = exact_final);
        // The public refresh entry must finish without any provider result,
        // transport, operating unlock or new authorization.
        const auto refreshed = paymaster_app::ResolvePaymasterSession(context, client,
            paymaster_app::ClientSessionLookup{session.request_id}, "refresh", {});
        BOOST_REQUIRE(refreshed.session.session);
        BOOST_CHECK(refreshed.session.session->session.state == SessionState::CANCELED_SAFE);
        BOOST_REQUIRE(store.GetSessionByRequestId(session.request_id, session));
        BOOST_CHECK(session.state == SessionState::CANCELED_SAFE);
        BOOST_CHECK(session.recovery_txid == CTransaction{final_transaction}.GetHash());
        BOOST_CHECK(session.final_txid.IsNull());
        BOOST_CHECK(store.GetAlternativeRecoveryWithStatus(session.request_id, recovery) == DatabaseReadStatus::NOT_FOUND);
        BOOST_REQUIRE(WalletBatch{client.GetDatabase()}.ReadPaymasterClientFeeLedger(ledger));
        const auto spent = std::find_if(ledger.reservations.begin(), ledger.reservations.end(), [&](const auto& fee) {
            return fee.commit_key == recovery_fee_key;
        });
        BOOST_REQUIRE(spent != ledger.reservations.end());
        BOOST_CHECK(spent->state == BudgetReservationState::SPENT);
        const auto released_fee = std::find_if(ledger.reservations.begin(), ledger.reservations.end(), [&](const auto& fee) {
            return fee.commit_key == original_attempt.commit_key;
        });
        BOOST_REQUIRE(released_fee != ledger.reservations.end());
        BOOST_CHECK(released_fee->state == BudgetReservationState::RELEASED);
        BOOST_CHECK(!IsPaymasterInputReserved(client, reserved_input));
        PaymasterSessionObservation observation;
        BOOST_REQUIRE_MESSAGE(store.GetSessionObservation(session, observation, error), error);
        BOOST_CHECK(!observation.PaymentConfirmed(session.state));
        BOOST_CHECK(observation.Status(session.state) == "canceled");
        const auto settled = failed_database.m_records;
        BOOST_REQUIRE(store.CompleteClientConfirmedFinal(session.request_id, GetTime(), completed, error));
        BOOST_CHECK(!completed);
        BOOST_CHECK(failed_database.m_records == settled);
        return;
    }
    const auto encoded = SerializePaymasterSecurityObject(result);
    const uint256 message_id{Hash(encoded)};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(45, message_id, encoded.size(), DirectPayload{result}, now));
    const auto invoke = [&] {
        return paymaster_app::ResolveAlternativePaymasterRecovery(context, client, store, session, original_attempt, {});
    };
    auto& database = GetMockableDatabase(client);
    const auto before = database.m_records;
    const AlternativeRecoveryRecord authorized_recovery{recovery};
    for (int scenario = 0; scenario < 3; ++scenario) {
        BOOST_TEST_CONTEXT("readable corrupt recovery authority " << scenario) {
            AlternativeRecoveryRecord damaged{authorized_recovery};
            if (scenario == 0) damaged.recovery_response.recovery_commit_key = SecurityTestId(seed, 127);
            if (scenario == 1) damaged.recovery_request.original_commit_key = SecurityTestId(seed, 128);
            if (scenario == 2) damaged.recovery_response.identity_signature.back() ^= 1;
            {
                LOCK(client.cs_wallet);
                BOOST_REQUIRE(WalletBatch{client.GetDatabase()}.WritePaymasterAlternativeRecovery(damaged));
            }
            AlternativeRecoveryRecord readable;
            BOOST_REQUIRE(store.GetAlternativeRecoveryWithStatus(session.request_id, readable) == DatabaseReadStatus::FOUND);
            const auto damaged_rows = database.m_records;
            BOOST_CHECK_EXCEPTION(invoke(), ApplicationError,
                [](const ApplicationError& failure) {
                    return failure.kind == ErrorKind::WALLET && failure.code == "PAYMASTER_RECOVERY_DATABASE_READ" &&
                           failure.disposition == ErrorDisposition::RECONCILE && !failure.session_absent;
                });
            BOOST_CHECK(database.m_records == damaged_rows);
            BOOST_CHECK(IsPaymasterInputReserved(client, session.user_inputs.front()));
            manager.PruneDirectMessages(now + DIRECT_REPLAY_TTL_SECONDS + MAX_DIRECT_MESSAGE_TTL_SECONDS + 1);
            BOOST_REQUIRE_EQUAL(manager.DirectMessageCount(), 1U);
            database.m_records = before;
        }
    }
    // Intact local authority still permits rejection of a forged remote
    // signature. It never receives authenticated-result TTL protection.
    {
        Manager invalid_manager{true};
        context.paymaster = &invalid_manager;
        auto invalid_result = result;
        invalid_result.result.identity_signature.back() ^= 1;
        const auto invalid_bytes = SerializePaymasterSecurityObject(invalid_result);
        BOOST_REQUIRE(invalid_manager.EnqueueDirectMessage(45, Hash(invalid_bytes), invalid_bytes.size(), DirectPayload{invalid_result}, now));
        BOOST_CHECK_EXCEPTION(invoke(), ApplicationError,
            [](const ApplicationError& failure) {
                return failure.kind == ErrorKind::INVALID_INPUT && failure.code == "PAYMASTER_INVALID_RESULT_SIGNATURE";
            });
        BOOST_CHECK_EQUAL(invalid_manager.DirectMessageCount(), 0U);
        BOOST_CHECK(database.m_records == before);
        context.paymaster = &manager;
    }
    database.FailCommit();
    BOOST_CHECK_EXCEPTION(invoke(), ApplicationError,
        [](const ApplicationError& failure) { return failure.code == "PAYMASTER_DATABASE_COMMIT"; });
    database.ClearFailureInjection();
    BOOST_CHECK(database.m_records == before);
    BOOST_REQUIRE(store.GetAlternativeRecovery(session.request_id, recovery));
    BOOST_CHECK(recovery.phase == AlternativeRecoveryPhase::USER_SIGNED);
    BOOST_CHECK(IsPaymasterInputReserved(client, session.user_inputs.front()));
    BOOST_REQUIRE_EQUAL(manager.DirectMessageCount(), 1U);
    SetMockTime(now + DIRECT_REPLAY_TTL_SECONDS + MAX_DIRECT_MESSAGE_TTL_SECONDS + 1);
    manager.PruneDirectMessages(GetTime());
    auto unrelated = request.capacity_request;
    unrelated.session_id = SecurityTestId(seed, 126);
    const auto unrelated_bytes = SerializePaymasterSecurityObject(unrelated);
    const uint256 unrelated_id{Hash(unrelated_bytes)};
    BOOST_REQUIRE(manager.EnqueueDirectMessage(46, unrelated_id, unrelated_bytes.size(), DirectPayload{unrelated}, GetTime()));
    BOOST_REQUIRE(manager.AcknowledgeDirectMessagesIfPresent({unrelated_id}));
    BOOST_REQUIRE_EQUAL(manager.DirectMessageCount(), 1U);
    const auto retried = invoke();
    BOOST_CHECK(retried.broadcast.value_or(false));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
    BOOST_REQUIRE(store.GetAlternativeRecovery(session.request_id, recovery));
    BOOST_CHECK(recovery.phase == AlternativeRecoveryPhase::FINAL_COMMITTED);
    BOOST_CHECK(recovery.final_transaction == SerializePaymasterSecurityObject(final_transaction));
    BOOST_CHECK(SerializePaymasterSecurityObject(recovery.signed_result) == SerializePaymasterSecurityObject(result.result));
    SelfRecoveryRecord saved;
    BOOST_REQUIRE(store.GetSelfRecovery(session.request_id, saved));
    BOOST_CHECK(saved.final_transaction == recovery.final_transaction);
    BOOST_CHECK(saved.recovery_txid == *result.result.txid);
    if (confirmed_before_result) {
        BOOST_REQUIRE(store.GetSessionByRequestId(session.request_id, session));
        BOOST_CHECK(session.state == SessionState::CANCELED_SAFE);
        BOOST_CHECK(session.final_txid.IsNull());
        BOOST_CHECK(session.recovery_txid == *result.result.txid);
        BOOST_REQUIRE(retried.session.session);
        BOOST_CHECK(retried.session.session->observation.payment.confirmations == 0);
        BOOST_CHECK(retried.session.session->observation.recovery.confirmations > 0);
    } else {
        BOOST_REQUIRE(m_node.mempool->get(*result.result.txid));
    }
    BOOST_REQUIRE(WalletBatch{client.GetDatabase()}.ReadPaymasterClientFeeLedger(ledger));
    BOOST_REQUIRE_MESSAGE(ValidateClientFeeLedger(ledger, error), error);
    BOOST_REQUIRE_EQUAL(ledger.reservations.size(), confirmed_before_result ? 2U : 1U);
    const auto recovery_fee = std::find_if(ledger.reservations.begin(), ledger.reservations.end(),
        [&](const ClientFeeReservation& entry) { return entry.commit_key == response.recovery_commit_key; });
    BOOST_REQUIRE(recovery_fee != ledger.reservations.end());
    BOOST_CHECK(recovery_fee->state == BudgetReservationState::SPENT);
    BOOST_CHECK_EQUAL(recovery_fee->service_fee.value, 100);
    BOOST_CHECK_EQUAL(recovery_fee->reserved_at, recovery.recovery_authorization_accepted_at);
    BOOST_CHECK(recovery_fee->updated_at >= recovery_fee->reserved_at);
    if (confirmed_before_result) {
        BOOST_REQUIRE(original_attempt.commit_key != response.recovery_commit_key);
        const auto original_fee = std::find_if(ledger.reservations.begin(), ledger.reservations.end(),
            [&](const ClientFeeReservation& entry) { return entry.commit_key == original_attempt.commit_key; });
        BOOST_REQUIRE(original_fee != ledger.reservations.end());
        // One confirmation does not release a still-recoverable original
        // signature. Its sponsored liability remains reserved at exactly zero.
        BOOST_CHECK(original_fee->state == BudgetReservationState::RESERVED);
        BOOST_CHECK_EQUAL(original_fee->service_fee.value, original_attempt.client_manifest.service_fee.value);
        BOOST_CHECK_EQUAL(original_fee->service_fee.value, 0);
        BOOST_CHECK_EQUAL(original_fee->reserved_at, original_attempt.client_manifest_accepted_at);
        BOOST_CHECK(original_fee->updated_at >= original_fee->reserved_at);
    }
    CAmount reserved_fees{0}, spent_fees{0}, released_fees{0};
    for (const auto& entry : ledger.reservations) {
        if (entry.state == BudgetReservationState::RESERVED) reserved_fees += entry.service_fee.value;
        if (entry.state == BudgetReservationState::SPENT) spent_fees += entry.service_fee.value;
        if (entry.state == BudgetReservationState::RELEASED) released_fees += entry.service_fee.value;
    }
    BOOST_CHECK_EQUAL(reserved_fees, 0);
    BOOST_CHECK_EQUAL(spent_fees, 100);
    BOOST_CHECK_EQUAL(released_fees, 0);
    const auto spent_ledger = SerializePaymasterSecurityObject(ledger);
    const auto replay = invoke();
    BOOST_CHECK(replay.broadcast.value_or(false));
    BOOST_REQUIRE(WalletBatch{client.GetDatabase()}.ReadPaymasterClientFeeLedger(ledger));
    BOOST_CHECK(SerializePaymasterSecurityObject(ledger) == spent_ledger);
    if (confirmed_before_result) {
        wallet::CWallet reopened{m_node.chain.get(), "recovery-result-reopened",
            DuplicateSecurityFixtureDatabase(client.GetDatabase())};
        BOOST_REQUIRE(reopened.LoadWallet() == DBErrors::LOAD_OK);
        const auto tip_hash = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        const int tip_height = *m_node.chain->getHeight();
        {
            LOCK(reopened.cs_wallet);
            reopened.SetLastBlockProcessed(tip_height, tip_hash);
        }
        PaymasterStore reopened_store{reopened};
        PaymentSession reopened_session;
        BOOST_REQUIRE(reopened_store.GetSessionByRequestId(session.request_id, reopened_session));
        const auto resumed = paymaster_app::ResolveAlternativePaymasterRecovery(context, reopened,
            reopened_store, reopened_session, original_attempt, {});
        BOOST_CHECK(resumed.broadcast.value_or(false));
        BOOST_REQUIRE(resumed.session.session);
        BOOST_CHECK(resumed.session.session->session.state == SessionState::CANCELED_SAFE);
        BOOST_CHECK(resumed.session.session->session.final_txid.IsNull());
        BOOST_CHECK(resumed.session.session->observation.payment.confirmations == 0);
        BOOST_CHECK(resumed.session.session->observation.recovery.confirmations > 0);
        ClientFeeLedger reloaded_ledger;
        BOOST_REQUIRE(WalletBatch{reopened.GetDatabase()}.ReadPaymasterClientFeeLedger(reloaded_ledger));
        BOOST_CHECK(SerializePaymasterSecurityObject(reloaded_ledger) == spent_ledger);
    }
}

BOOST_FIXTURE_TEST_CASE(client_recovery_result_commit_failure_survives_ttl_and_exact_retry,
                        ClientRecoveryReadSetup)
{
    ExerciseClientRecoveryResult(*this, /*confirmed_before_result=*/false);
}

BOOST_FIXTURE_TEST_CASE(client_recovery_quote_intake_retains_local_failures_and_retries_before_signing,
                        ClientRecoveryReadSetup)
{
    ExerciseClientRecoveryResult(*this, /*confirmed_before_result=*/false,
        /*exercise_provider_history=*/false, /*exercise_quote_intake=*/true);
}

BOOST_FIXTURE_TEST_CASE(client_recovery_confirmed_before_first_result_survives_expiry_and_reload,
                        ClientRecoveryReadSetup)
{
    ExerciseClientRecoveryResult(*this, /*confirmed_before_result=*/true);
}

BOOST_FIXTURE_TEST_CASE(client_recovery_mempool_final_without_result_retries_exact_authorization,
                        ClientRecoveryReadSetup)
{
    ExerciseClientRecoveryResult(*this, /*confirmed_before_result=*/false,
        /*exercise_provider_history=*/false, /*exercise_quote_intake=*/false,
        /*observe_without_result=*/true);
}

BOOST_FIXTURE_TEST_CASE(client_recovery_permanent_provider_loss_settles_only_after_safety_depth,
                        ClientRecoveryReadSetup)
{
    ExerciseClientRecoveryResult(*this, true, false, false, false, true);
}

BOOST_FIXTURE_TEST_CASE(provider_confirmed_recovery_archives_only_after_reorg_safety_depth,
                        ClientRecoveryReadSetup)
{
    ExerciseClientRecoveryResult(*this, /*confirmed_before_result=*/true,
        /*exercise_provider_history=*/true);
}

BOOST_FIXTURE_TEST_CASE(provider_high_privacy_recovery_uses_saved_arrival_observation,
                        QuoteRequestProcessingSetup)
{
    auto request = MakeRecoveryReadRequest(environment, seed, now);
    request.privacy_profile = PrivacyProfile::HIGH;
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoveryRequestEnvelope(request,
        Params().GenesisBlock().GetHash(), now, error), error);
    const auto encoded = SerializePaymasterSecurityObject(request);
    const auto before = GetMockableDatabase(wallet).m_records;
    const auto checkpoint_before = CheckpointBytes();
    for (const auto observed : {std::optional<bool>{}, std::optional<bool>{false}, std::optional<bool>{true}}) {
        BOOST_TEST_CONTEXT("known arrival " << observed.has_value() << ", onion " << observed.value_or(false)) {
            Manager transport{true};
            context.paymaster = &transport;
            BOOST_REQUIRE(transport.StartProvider(wallet.GetName(), environment.identity.provider_id));
            BOOST_REQUIRE(transport.EnqueueNetworkDirectMessage(44, Hash(encoded), encoded.size(),
                DirectPayload{request}, now, 44, {1, 8, 8}, DirectAdmissionClass::REQUEST,
                observed) == DirectEnqueueResult::ACCEPTED);
            transport.ForgetDirectPeer(44);
            if (!observed) {
                BOOST_CHECK_EXCEPTION(paymaster_app::ProcessPaymasterRequests(context, wallet, paymaster_app::ExecutionMode::MANUAL),
                    ApplicationError, [](const ApplicationError& failure) {
                        return failure.kind == ErrorKind::WALLET &&
                            failure.code == "PAYMASTER_RECOVERY_TRANSPORT_OBSERVATION_UNAVAILABLE";
                    });
                BOOST_CHECK_EQUAL(transport.DirectMessageCount(), 1U);
            } else {
                const auto result = paymaster_app::ProcessPaymasterRequests(context, wallet, paymaster_app::ExecutionMode::MANUAL);
                BOOST_CHECK(result.IsRejected());
                // Onion passes the transport gate and reaches the ordinary
                // policy check; this fixture deliberately has another policy.
                BOOST_CHECK_EQUAL(result.rejection_reason.value_or(""), *observed
                    ? "PAYMASTER_RECOVERY_POLICY_MISMATCH"
                    : "PAYMASTER_RECOVERY_HIGH_PRIVACY_REQUIRES_ONION");
                BOOST_CHECK_EQUAL(transport.DirectMessageCount(), 0U);
            }
            BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);
            BOOST_CHECK(CheckpointBytes() == checkpoint_before);
            context.paymaster = &manager;
        }
    }
}

BOOST_FIXTURE_TEST_CASE(provider_recovery_read_failure_retains_request_and_replay,
                        QuoteRequestProcessingSetup)
{
    const auto request = MakeRecoveryReadRequest(environment, seed, now);
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoveryRequestEnvelope(request,
        Params().GenesisBlock().GetHash(), now, error), error);
    const auto record = MakeRecoveryReadReplay(environment, request, now);
    const auto key = std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, record.recovery_id);
    const auto encoded = SerializePaymasterSecurityObject(request);
    const uint256 message_id{Hash(encoded)};
    auto& database = GetMockableDatabase(wallet);
    const auto absent_rows = database.m_records;
    for (int scenario = 0; scenario < 4; ++scenario) {
        BOOST_TEST_CONTEXT("provider recovery read scenario " << scenario) {
            database.m_records = absent_rows;
            Manager transport{true};
            context.paymaster = &transport;
            BOOST_REQUIRE(transport.StartProvider(wallet.GetName(), environment.identity.provider_id));
            BOOST_REQUIRE(transport.CanReceiveDirectMessage(44, DirectPayload{request}, DirectAdmissionClass::REQUEST, now));
            BOOST_REQUIRE(transport.EnqueueNetworkDirectMessage(44, message_id, encoded.size(),
                DirectPayload{request}, now, 44, {1, 8, 8}, DirectAdmissionClass::REQUEST) == DirectEnqueueResult::ACCEPTED);
            {
                LOCK(wallet.cs_wallet);
                auto batch = wallet.GetDatabase().MakeBatch();
                if (scenario < 2) {
                    BOOST_REQUIRE(batch->Write(key, uint16_t(AlternativeRecoveryRecord::CURRENT_VERSION + scenario)));
                } else if (scenario == 3) {
                    BOOST_REQUIRE(batch->Write(key, record));
                }
            }
            const auto before = database.m_records;
            const auto checkpoint_before = CheckpointBytes();
            if (scenario == 3) {
                const auto replay = paymaster_app::ProcessPaymasterRequests(context, wallet, paymaster_app::ExecutionMode::MANUAL);
                BOOST_CHECK(replay.HasMessage());
                BOOST_CHECK(replay.queued.value_or(false));
                BOOST_CHECK(transport.HasOutboundDirectMessage(44, Hash(SerializePaymasterSecurityObject(record.recovery_response))));
                const auto lease = transport.LeaseRecoveryRequests(environment.identity.provider_id, 1);
                BOOST_CHECK(lease.Messages().empty());
            } else if (scenario == 2) {
                // Verified absence reaches a permanent remote policy mismatch.
                // It must not receive the retry treatment of unreadable rows.
                const auto result = paymaster_app::ProcessPaymasterRequests(context, wallet, paymaster_app::ExecutionMode::MANUAL);
                BOOST_CHECK(result.HasMessage());
                BOOST_CHECK(result.IsRejected());
                BOOST_CHECK_EQUAL(result.rejection_reason.value_or(""), "PAYMASTER_RECOVERY_POLICY_MISMATCH");
                BOOST_CHECK_EQUAL(transport.DirectMessageCount(), 0U);
                BOOST_CHECK_EQUAL(transport.OutboundDirectMessageCount(), 0U);
            } else {
                const char* expected = scenario == 0 ? "PAYMASTER_RECOVERY_DATABASE_READ" : "PAYMASTER_UNSUPPORTED_PERSISTED_VERSION";
                BOOST_CHECK_EXCEPTION(paymaster_app::ProcessPaymasterRequests(context, wallet, paymaster_app::ExecutionMode::MANUAL),
                    ApplicationError, [&](const ApplicationError& failure) {
                        return failure.code == expected && failure.disposition == ErrorDisposition::RECONCILE &&
                            failure.kind == ErrorKind::WALLET;
                    });
                const auto lease = transport.LeaseRecoveryRequests(environment.identity.provider_id, 1);
                BOOST_REQUIRE_EQUAL(lease.Messages().size(), 1U);
                BOOST_CHECK(lease.Messages().front().message_id == message_id);
                BOOST_CHECK_EQUAL(transport.DirectMessageCount(), 1U);
                BOOST_CHECK_EQUAL(transport.OutboundDirectMessageCount(), 0U);
            }
            BOOST_CHECK(database.m_records == before);
            BOOST_CHECK(CheckpointBytes() == checkpoint_before);
            context.paymaster = &manager;
        }
    }
}

BOOST_FIXTURE_TEST_CASE(provider_capacity_lookup_distinguishes_peer_conflicts_from_local_damage,
                        QuoteRequestProcessingSetup)
{
    PaymasterStore store{wallet};
    PaymasterCapacityProof proof;
    std::string error;
    const auto read = [&](const PaymasterCapacityRequest& request, int64_t at) {
        return store.GetProviderCapacityProofWithStatus(request, at, proof, error);
    };
    BOOST_REQUIRE(read(fixture.capacity_request, now) == ProviderCapacityProofStatus::FOUND);
    BOOST_CHECK(SerializePaymasterSecurityObject(proof) == SerializePaymasterSecurityObject(fixture.capacity_proof));
    BOOST_CHECK(read(fixture.capacity_request, fixture.capacity_request.expires_at) == ProviderCapacityProofStatus::EXPIRED);

    auto other = fixture.capacity_request;
    other.client_nonce = SecurityTestId(seed, 120);
    BOOST_CHECK(read(other, now) == ProviderCapacityProofStatus::REQUEST_MISMATCH);
    other.session_id = SecurityTestId(seed, 121);
    BOOST_CHECK(read(other, now) == ProviderCapacityProofStatus::NOT_FOUND);
    BOOST_CHECK(paymaster_app::IsPermanentCapacityContinuationError(error));
    BOOST_CHECK(proof.snapshot_id.IsNull());

    auto& database = GetMockableDatabase(wallet);
    const auto intact = database.m_records;
    const auto nonce_key = std::make_pair(DBKeys::PAYMASTER_CAPACITY_NONCE, fixture.capacity_request.client_nonce);
    const auto response_key = std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESPONSE, fixture.capacity_request_hash);
    for (int scenario = 0; scenario < 5; ++scenario) {
        BOOST_TEST_CONTEXT("capacity local damage " << scenario) {
            database.m_records = intact;
            auto batch = database.MakeBatch();
            if (scenario == 0) BOOST_REQUIRE(batch->Write(nonce_key, uint8_t{0}));
            if (scenario == 1) BOOST_REQUIRE(batch->Write(response_key, std::vector<unsigned char>{1}));
            if (scenario == 2) BOOST_REQUIRE(batch->Erase(response_key));
            if (scenario == 3) BOOST_REQUIRE(batch->Erase(nonce_key));
            if (scenario == 4) {
                auto damaged = fixture.capacity_proof;
                damaged.identity_signature.front() ^= 1;
                BOOST_REQUIRE(batch->Write(response_key, SerializePaymasterSecurityObject(damaged)));
            }
            const auto before = database.m_records;
            BOOST_CHECK(read(fixture.capacity_request, now) == ProviderCapacityProofStatus::READ_ERROR);
            // Ordinary quote admission retains the boolean compatibility API.
            // Its peer classifier must preserve the same local corruption.
            BOOST_CHECK(!paymaster_app::IsPermanentCapacityContinuationError(error));
            BOOST_CHECK(proof.snapshot_id.IsNull());
            BOOST_CHECK(database.m_records == before);
        }
    }
    database.m_records = intact;
}

BOOST_FIXTURE_TEST_CASE(provider_recovery_capacity_absence_rejects_but_read_failures_retain_request,
                        QuoteRequestProcessingSetup)
{
    // Enable a valid public user-paid policy. This test stops at capacity
    // lookup, before a new signature or a readiness-dependent continuation.
    environment.policy.funding_models = FUNDING_MODEL_ALL;
    environment.safety.user_paid = SecurityLimits(2000);
    environment.settings.policy_hash = GetProviderPolicyHash(environment.policy);
    {
        WalletBatch batch{wallet.GetDatabase()};
        BOOST_REQUIRE(batch.WritePaymasterPolicy(environment.policy));
        BOOST_REQUIRE(batch.WritePaymasterProviderSafetyPolicy(environment.safety));
        BOOST_REQUIRE(batch.WritePaymasterSettings(environment.settings));
    }
    auto request = MakeRecoveryReadRequest(environment, seed, now);
    request.policy_hash = GetProviderPolicyHash(environment.policy);
    HashWriter offer_hasher = TaggedHash("DigiByte Paymaster Offer v1");
    offer_hasher << environment.identity.provider_id << request.policy_hash
                 << static_cast<uint8_t>(FundingModel::USER_PAID);
    request.offer_id = offer_hasher.GetSHA256();
    const auto bytes = SerializePaymasterSecurityObject(request);
    const auto request_hash = Hash(SerializePaymasterSecurityObject(request.capacity_request));
    auto& database = GetMockableDatabase(wallet);
    const auto intact = database.m_records;
    for (int scenario = 0; scenario < 4; ++scenario) {
        BOOST_TEST_CONTEXT("recovery capacity read " << scenario) {
            database.m_records = intact;
            Manager transport{true};
            context.paymaster = &transport;
            BOOST_REQUIRE(transport.StartProvider(wallet.GetName(), environment.identity.provider_id));
            BOOST_REQUIRE(transport.EnqueueNetworkDirectMessage(44, Hash(bytes), bytes.size(),
                DirectPayload{request}, now, 44, {1, 8, 8}, DirectAdmissionClass::REQUEST) == DirectEnqueueResult::ACCEPTED);
            if (scenario != 0) {
                auto batch = database.MakeBatch();
                const auto nonce_key = std::make_pair(DBKeys::PAYMASTER_CAPACITY_NONCE, request.client_nonce);
                if (scenario == 1) {
                    BOOST_REQUIRE(batch->Write(nonce_key, uint8_t{0}));
                } else {
                    BOOST_REQUIRE(batch->Write(nonce_key, request_hash));
                    if (scenario == 2) {
                        BOOST_REQUIRE(batch->Write(std::make_pair(DBKeys::PAYMASTER_CAPACITY_RESPONSE, request_hash),
                            std::vector<unsigned char>{1}));
                    }
                }
            }
            const auto before = database.m_records;
            const auto checkpoint_before = CheckpointBytes();
            if (scenario == 0) {
                const auto result = paymaster_app::ProcessPaymasterRequests(context, wallet, paymaster_app::ExecutionMode::MANUAL);
                BOOST_CHECK(result.IsRejected());
                BOOST_CHECK_EQUAL(result.rejection_reason.value_or(""), "PAYMASTER_CAPACITY_RESERVATION_MISSING");
                BOOST_CHECK_EQUAL(transport.DirectMessageCount(), 0U);
            } else {
                BOOST_CHECK_EXCEPTION(paymaster_app::ProcessPaymasterRequests(context, wallet, paymaster_app::ExecutionMode::MANUAL),
                    ApplicationError, [](const ApplicationError& failure) {
                        return failure.kind == ErrorKind::WALLET && failure.disposition == ErrorDisposition::RECONCILE;
                    });
                const auto lease = transport.LeaseRecoveryRequests(environment.identity.provider_id, 1);
                BOOST_REQUIRE_EQUAL(lease.Messages().size(), 1U);
                BOOST_CHECK(lease.Messages().front().message_id == Hash(bytes));
            }
            BOOST_CHECK_EQUAL(transport.OutboundDirectMessageCount(), 0U);
            BOOST_CHECK(database.m_records == before);
            BOOST_CHECK(CheckpointBytes() == checkpoint_before);
            context.paymaster = &manager;
        }
    }
    database.m_records = intact;
}

static void AddPendingOperationalTarget(QuoteRequestProcessingSetup& setup)
{
    // Service fairness runs while a useful target is pending. The reserved
    // quote's 2,000-satoshi input is below the configured target/fee minimum.
    CMutableTransaction pending;
    pending.vin.emplace_back(COutPoint{SecurityTestId(setup.seed, 195), 0});
    pending.vout.emplace_back(setup.environment.policy.maximum_network_fee.value,
                             setup.environment.identity.identity_script);
    const auto transaction = MakeTransactionRef(pending);
    LOCK(setup.wallet.cs_wallet);
    BOOST_REQUIRE(setup.wallet.AddToWallet(transaction, TxStateInMempool{}));
    WalletBatch batch{setup.wallet.GetDatabase()};
    std::vector<ProviderPoolEntry> pool;
    BOOST_REQUIRE(batch.ReadPaymasterProviderPool(pool));
    ProviderPoolEntry entry;
    entry.outpoint = COutPoint{transaction->GetHash(), 0};
    entry.purpose = PoolPurpose::OPERATIONAL;
    entry.script_pub_key = pending.vout.front().scriptPubKey;
    entry.dgb_value = DGBSatoshis{pending.vout.front().nValue};
    entry.updated_at = GetTime();
    pool.push_back(entry);
    BOOST_REQUIRE(batch.WritePaymasterProviderPool(pool));
}

BOOST_FIXTURE_TEST_CASE(automatic_recovery_rejection_allows_following_quote_and_submit,
                        QuoteRequestProcessingSetup)
{
    environment.settings.operation_mode = ProviderOperationMode::AUTOMATIC;
    BOOST_REQUIRE(WalletBatch{wallet.GetDatabase()}.WritePaymasterSettings(environment.settings));
    // This test expects confirmation waiting, so supply the same pending
    // operational target as the other automatic-service fairness fixtures.
    AddPendingOperationalTarget(*this);
    RunPaymasterProviderServiceCycle(context, wallet);
    const auto initial = manager.GetProviderServiceStatus(wallet.GetName());
    BOOST_REQUIRE_MESSAGE(initial.state == ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION,
        "state=" << static_cast<int>(initial.state) << " error=" << initial.last_error);

    const auto recovery = MakeRecoveryReadRequest(environment, seed, now);
    std::string error;
    BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoveryRequestEnvelope(recovery,
        Params().GenesisBlock().GetHash(), now, error), error);
    const auto bytes = SerializePaymasterSecurityObject(recovery);
    BOOST_REQUIRE(manager.EnqueueNetworkDirectMessage(44, Hash(bytes), bytes.size(), DirectPayload{recovery},
        now, 44, {1, 8, 8}, DirectAdmissionClass::REQUEST) == DirectEnqueueResult::ACCEPTED);
    EnqueueQuote(manager, fixture.request);
    PaymasterSubmit stale;
    stale.genesis_hash = Params().GenesisBlock().GetHash();
    stale.request_id = "550e8400-e29b-41d4-a716-446655441100";
    stale.session_id = SecurityTestId(seed, 130);
    stale.provider_id = environment.identity.provider_id;
    stale.quote_id = SecurityTestId(seed, 131);
    stale.commit_key = SecurityTestId(seed, 132);
    stale.template_commitment = SecurityTestId(seed, 133);
    stale.user_psbt = {1};
    BOOST_REQUIRE_MESSAGE(ValidateSubmitEnvelope(stale, stale.genesis_hash, error), error);
    const auto submit_bytes = SerializePaymasterSecurityObject(stale);
    BOOST_REQUIRE(manager.EnqueueNetworkDirectMessage(45, Hash(submit_bytes), submit_bytes.size(), DirectPayload{stale},
        now, 45, {1, 9, 9}, DirectAdmissionClass::REQUEST) == DirectEnqueueResult::ACCEPTED);
    const auto before = GetMockableDatabase(wallet).m_records;
    const auto checkpoint_before = CheckpointBytes();
    for (int pass = 0; pass < 2; ++pass) {
        RunPaymasterProviderServiceCycle(context, wallet);
        const auto status = manager.GetProviderServiceStatus(wallet.GetName());
        BOOST_CHECK(status.state == initial.state);
        BOOST_CHECK_EQUAL(status.last_error, initial.last_error);
    }
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
    const auto replies = manager.TakeOutboundDirectMessages(44, 8, GetTime());
    BOOST_REQUIRE_EQUAL(replies.size(), 1U);
    const auto* quote = std::get_if<PaymasterQuoteResponse>(&replies.front().payload);
    BOOST_REQUIRE(quote);
    BOOST_CHECK(SerializePaymasterSecurityObject(*quote) == fixture.attempt.signed_quote);
    BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);
    BOOST_CHECK(CheckpointBytes() == checkpoint_before);
}

struct SignedPaymentProcessingSetup : QuoteRequestProcessingSetup {
    // The valid-final path must meet DigiByte's relay fee, unlike classifier fixtures.
    SignedPaymentProcessingSetup() : QuoteRequestProcessingSetup(0, DGBSatoshis{20000}) {}
};

BOOST_FIXTURE_TEST_CASE(automatic_pending_setup_processes_valid_signed_payment, SignedPaymentProcessingSetup)
{
    std::string error;
    BOOST_REQUIRE_MESSAGE(AddTaprootSigningKey(wallet, fixture.user_key, error), error);
    {
        LOCK(wallet.cs_wallet);
        BOOST_REQUIRE(wallet.AddToWallet(MakeTransactionRef(CMutableTransaction{
            fixture.response.quote.reserved_dgb_inputs.front().creating_tx}), TxStateInMempool{}));
    }
    CollaborativePSBTTemplate trusted;
    BOOST_REQUIRE(DecodeRawPSBT(trusted.psbt, MakeByteSpan(fixture.attempt.unsigned_psbt), error));
    trusted.input_roles = {InputRole::USER_DD, InputRole::PROVIDER_DGB};
    PartiallySignedTransaction user_psbt{trusted.psbt};
    BOOST_REQUIRE_MESSAGE(SignCollaborativePSBTForParty(wallet, user_psbt, trusted, SigningParty::USER, error), error);
    BOOST_REQUIRE_MESSAGE(ValidateCollaborativePSBT(user_psbt, trusted, CollaborativeSignatureStage::USER_SIGNED, error), error);
    {
        LOCK(cs_main);
        auto& coins = m_node.chainman->ActiveChainstate().CoinsTip();
        for (size_t i{0}; i < trusted.psbt.inputs.size(); ++i) {
            CTxOut prevout;
            BOOST_REQUIRE(trusted.psbt.GetInputUTXO(prevout, i));
            coins.AddCoin(fixture.transaction.vin[i].prevout, Coin{std::move(prevout), 0, false}, false);
        }
    }
    environment.settings.operation_mode = ProviderOperationMode::AUTOMATIC;
    WalletBatch batch{wallet.GetDatabase()};
    BOOST_REQUIRE(batch.WritePaymasterSettings(environment.settings));
    ProviderMaintenanceRecord preparation;
    preparation.kind = ProviderMaintenanceKind::PREPARE_DGB;
    preparation.operation_id = SecurityTestId(seed, 190);
    preparation.plan_id = SecurityTestId(seed, 191);
    preparation.preparation_authorization = SecurityTestId(seed, 192);
    preparation.preparation_request = SecurityTestId(seed, 193);
    preparation.maximum_fee = DGBSatoshis{2000};
    preparation.created_at = preparation.updated_at = GetTime();
    ProviderMaintenanceOutput output;
    output.script_pub_key = environment.identity.identity_script;
    output.dgb_value = DGBSatoshis{10000};
    preparation.outputs = {output};
    ProviderMaintenanceLedger maintenance;
    maintenance.records = {preparation};
    BOOST_REQUIRE_MESSAGE(ValidateProviderMaintenanceLedger(maintenance, error), error);
    BOOST_REQUIRE(batch.WritePaymasterMaintenanceLedger(maintenance));
    PaymasterSubmit submit;
    submit.genesis_hash = Params().GenesisBlock().GetHash();
    submit.request_id = fixture.request.intent.request_id;
    submit.session_id = fixture.attempt.session_id;
    submit.provider_id = fixture.attempt.provider_id;
    submit.quote_id = fixture.attempt.quote_id;
    submit.commit_key = fixture.attempt.commit_key;
    submit.template_commitment = fixture.attempt.template_commitment;
    submit.user_psbt = SerializePaymasterSecurityObject(user_psbt);
    BOOST_REQUIRE_MESSAGE(ValidateSubmitEnvelope(submit, submit.genesis_hash, error), error);
    const auto bytes = SerializePaymasterSecurityObject(submit);
    BOOST_REQUIRE(manager.EnqueueNetworkDirectMessage(45, Hash(bytes), bytes.size(), DirectPayload{submit},
        GetTime(), 45, {1, 9, 9}, DirectAdmissionClass::REQUEST) == DirectEnqueueResult::ACCEPTED);
    RunPaymasterProviderServiceCycle(context, wallet);
    const auto status = manager.GetProviderServiceStatus(wallet.GetName());
    BOOST_CHECK_MESSAGE(status.state == ProviderServiceState::WAITING_FOR_READINESS, status.last_error);
    BOOST_CHECK_EQUAL(status.last_error, "PAYMASTER_POOL_PREPARATION_PENDING");
    ProviderCommitRecord committed;
    BOOST_REQUIRE_MESSAGE(batch.ReadPaymasterProviderCommit(submit.commit_key, committed), status.last_error);
    BOOST_CHECK_EQUAL(committed.final_txid, fixture.transaction.GetHash());
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 0U);
    const auto committed_bytes = SerializePaymasterSecurityObject(committed);
    RunPaymasterProviderServiceCycle(context, wallet);
    BOOST_REQUIRE(batch.ReadPaymasterProviderCommit(submit.commit_key, committed));
    BOOST_CHECK(SerializePaymasterSecurityObject(committed) == committed_bytes);
}

static void CheckAutomaticSubmitProgress(QuoteRequestProcessingSetup& setup, bool flood_recovery)
{
    auto& environment = setup.environment;
    auto& wallet = setup.wallet;
    auto& context = setup.context;
    auto& manager = setup.manager;
    const auto& seed = setup.seed;
    environment.settings.operation_mode = ProviderOperationMode::AUTOMATIC;
    BOOST_REQUIRE(WalletBatch{wallet.GetDatabase()}.WritePaymasterSettings(environment.settings));
    AddPendingOperationalTarget(setup);
    RunPaymasterProviderServiceCycle(context, wallet);
    const auto initial = manager.GetProviderServiceStatus(wallet.GetName());
    BOOST_REQUIRE(initial.state == ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION);
    const auto before = GetMockableDatabase(wallet).m_records;
    const auto checkpoint_before = setup.CheckpointBytes();

    const auto make_submit = [&](bool recovery, uint16_t sequence) -> DirectPayload {
        std::string error;
        if (recovery) {
            AlternativeRecoverySubmit submit;
            submit.genesis_hash = Params().GenesisBlock().GetHash();
            submit.request_id = "550e8400-e29b-41d4-a716-446655441201";
            submit.session_id = SecurityTestId(seed, sequence);
            submit.recovery_provider_id = environment.identity.provider_id;
            submit.recovery_id = SecurityTestId(seed, sequence + 1);
            submit.recovery_request_hash = SecurityTestId(seed, sequence + 2);
            submit.recovery_commit_key = SecurityTestId(seed, sequence + 3);
            submit.template_commitment = SecurityTestId(seed, sequence + 4);
            submit.user_psbt = {1};
            BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoverySubmitEnvelope(submit, submit.genesis_hash, error), error);
            return submit;
        }
        PaymasterSubmit submit;
        submit.genesis_hash = Params().GenesisBlock().GetHash();
        submit.request_id = "550e8400-e29b-41d4-a716-446655441200";
        submit.session_id = SecurityTestId(seed, sequence);
        submit.provider_id = environment.identity.provider_id;
        submit.quote_id = SecurityTestId(seed, sequence + 1);
        submit.commit_key = SecurityTestId(seed, sequence + 2);
        submit.template_commitment = SecurityTestId(seed, sequence + 3);
        submit.user_psbt = {1};
        BOOST_REQUIRE_MESSAGE(ValidateSubmitEnvelope(submit, submit.genesis_hash, error), error);
        return submit;
    };
    const auto enqueue = [&](DirectPayload payload, int64_t peer, uint64_t group) {
        const auto bytes = std::visit([](const auto& message) {
            return SerializePaymasterSecurityObject(message);
        }, payload);
        BOOST_REQUIRE(manager.AdmitDirectTransport(peer, group, GetTime(), DirectAdmissionClass::REQUEST));
        const auto admitted = manager.EnqueueNetworkDirectMessage(peer, Hash(bytes), bytes.size(),
            std::move(payload), GetTime(), group, {1, 8, static_cast<unsigned char>(group)},
            DirectAdmissionClass::REQUEST);
        BOOST_REQUIRE_MESSAGE(admitted == DirectEnqueueResult::ACCEPTED,
                              "network admission result=" << static_cast<int>(admitted));
    };

    uint16_t sequence{200};
    BOOST_TEST_CONTEXT("continuous recovery traffic=" << flood_recovery) {
        manager.ClearDirectMessages();
        // The marker is deliberately stale: being consumed proves it
        // reached classification, without granting payment authority.
        enqueue(make_submit(!flood_recovery, sequence), 45, 45);
        sequence += 5;
        for (int pass = 0; pass < 12; ++pass) {
            SetMockTime(GetTime() + 1);
            enqueue(make_submit(flood_recovery, sequence), 100 + pass % 4, 1000 + pass % 2);
            sequence += 5;
            const auto queued = manager.DirectMessageCount();
            RunPaymasterProviderServiceCycle(context, wallet);
            BOOST_CHECK_EQUAL(manager.DirectMessageCount(), queued - 1);
            const auto status = manager.GetProviderServiceStatus(wallet.GetName());
            BOOST_CHECK(status.state == initial.state);
            BOOST_CHECK_EQUAL(status.last_error, initial.last_error);
            BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);
            BOOST_CHECK(setup.CheckpointBytes() == checkpoint_before);
            // New competing work arrived on every cycle, yet the other
            // type was processed within two cycles in both directions.
            if (pass >= 1) {
                const auto remaining = flood_recovery
                    ? manager.LeaseSubmits(environment.identity.provider_id, MAX_DIRECT_INBOX_MESSAGES)
                    : manager.LeaseRecoverySubmits(environment.identity.provider_id, MAX_DIRECT_INBOX_MESSAGES);
                BOOST_REQUIRE(remaining.Messages().empty());
            }
        }
    }
}

BOOST_FIXTURE_TEST_CASE(automatic_submit_progress_under_continuous_recovery_traffic, QuoteRequestProcessingSetup)
{
    CheckAutomaticSubmitProgress(*this, /*flood_recovery=*/true);
}

BOOST_FIXTURE_TEST_CASE(automatic_recovery_progress_under_continuous_submit_traffic, QuoteRequestProcessingSetup)
{
    CheckAutomaticSubmitProgress(*this, /*flood_recovery=*/false);
}

static void CheckAutomaticRequestProgress(QuoteRequestProcessingSetup& setup, bool flood_recovery)
{
    auto& environment = setup.environment;
    auto& wallet = setup.wallet;
    auto& context = setup.context;
    auto& manager = setup.manager;
    const auto& seed = setup.seed;
    environment.settings.operation_mode = ProviderOperationMode::AUTOMATIC;
    BOOST_REQUIRE(WalletBatch{wallet.GetDatabase()}.WritePaymasterSettings(environment.settings));
    AddPendingOperationalTarget(setup);
    RunPaymasterProviderServiceCycle(context, wallet);
    const auto initial = manager.GetProviderServiceStatus(wallet.GetName());
    BOOST_REQUIRE(initial.state == ProviderServiceState::WAITING_FOR_LIQUIDITY_CONFIRMATION);
    const auto enqueue_recovery = [&](uint16_t sequence, int64_t peer, uint64_t group) {
        const auto request = MakeRecoveryReadRequest(environment, SecurityTestId(seed, sequence), GetTime());
        std::string error;
        BOOST_REQUIRE_MESSAGE(ValidateAlternativeRecoveryRequestEnvelope(request,
            Params().GenesisBlock().GetHash(), GetTime(), error), error);
        const auto bytes = SerializePaymasterSecurityObject(request);
        BOOST_REQUIRE(manager.AdmitDirectTransport(peer, group, GetTime(), DirectAdmissionClass::REQUEST));
        BOOST_REQUIRE(manager.EnqueueNetworkDirectMessage(peer, Hash(bytes), bytes.size(),
            DirectPayload{request}, GetTime(), group, {1, 8, static_cast<unsigned char>(group)},
            DirectAdmissionClass::REQUEST) == DirectEnqueueResult::ACCEPTED);
    };
    if (flood_recovery) setup.EnqueueQuote(manager, setup.fixture.request);
    else enqueue_recovery(590, 45, 45);
    const auto before = GetMockableDatabase(wallet).m_records;
    const auto checkpoint_before = setup.CheckpointBytes();

    for (int pass{0}; pass < 12; ++pass) {
        SetMockTime(GetTime() + 1);
        if (flood_recovery) {
            enqueue_recovery(600 + pass, 100 + pass % 4, 1000 + pass % 2);
        } else {
            auto request = setup.ConflictingRequest();
            request.intent.session_id = SecurityTestId(seed, 600 + pass);
            std::string error;
            BOOST_REQUIRE_MESSAGE(ValidateQuoteRequestEnvelope(request,
                Params().GenesisBlock().GetHash(), GetTime(), error), error);
            const auto bytes = SerializePaymasterSecurityObject(request);
            const int64_t peer = 100 + pass % 4;
            const uint64_t group = 1000 + pass % 2;
            BOOST_REQUIRE(manager.AdmitDirectTransport(peer, group, GetTime(), DirectAdmissionClass::REQUEST));
            BOOST_REQUIRE(manager.EnqueueNetworkDirectMessage(peer, Hash(bytes), bytes.size(),
                DirectPayload{request}, GetTime(), group, {1, 8, static_cast<unsigned char>(group)},
                DirectAdmissionClass::REQUEST) == DirectEnqueueResult::ACCEPTED);
        }
        const auto queued = manager.DirectMessageCount();
        RunPaymasterProviderServiceCycle(context, wallet);
        BOOST_CHECK_EQUAL(manager.DirectMessageCount(), queued - 1);
        const auto status = manager.GetProviderServiceStatus(wallet.GetName());
        BOOST_CHECK(status.state == initial.state);
        BOOST_CHECK_EQUAL(status.last_error, initial.last_error);
        BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);
        BOOST_CHECK(setup.CheckpointBytes() == checkpoint_before);
        if (pass >= 1) {
            const auto pending = flood_recovery
                ? manager.LeaseQuoteRequests(environment.identity.provider_id, MAX_DIRECT_INBOX_MESSAGES)
                : manager.LeaseRecoveryRequests(environment.identity.provider_id, MAX_DIRECT_INBOX_MESSAGES);
            BOOST_REQUIRE(pending.Messages().empty());
        }
    }
    if (flood_recovery) {
        const auto replies = manager.TakeOutboundDirectMessages(44, 1, GetTime());
        BOOST_REQUIRE_EQUAL(replies.size(), 1U);
        const auto* quote = std::get_if<PaymasterQuoteResponse>(&replies.front().payload);
        BOOST_REQUIRE(quote);
        BOOST_CHECK(SerializePaymasterSecurityObject(*quote) == setup.fixture.attempt.signed_quote);
    } else {
        BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 0U);
    }
}

BOOST_FIXTURE_TEST_CASE(automatic_quote_progress_under_continuous_recovery_requests, QuoteRequestProcessingSetup)
{
    CheckAutomaticRequestProgress(*this, /*flood_recovery=*/true);
}

BOOST_FIXTURE_TEST_CASE(automatic_recovery_request_progress_under_continuous_quotes, QuoteRequestProcessingSetup)
{
    CheckAutomaticRequestProgress(*this, /*flood_recovery=*/false);
}

BOOST_FIXTURE_TEST_CASE(deferred_capacity_preserves_request_and_yields_next_turn, QuoteRequestProcessingSetup)
{
    std::string error;
    // A separate free slot makes fresh capacity eligible. The original quote's
    // reserved slot and authority remain untouched by the throttling fixture.
    const int height = *m_node.chain->getHeight();
    const uint256 tip = m_node.chain->getBlockHash(height);
    CMutableTransaction funding;
    funding.vin.emplace_back(COutPoint{SecurityTestId(seed, 700), 0});
    funding.vout.emplace_back(COIN / 10, environment.identity.identity_script);
    const CTransactionRef funding_tx = MakeTransactionRef(funding);
    ProviderPoolEntry free_slot;
    free_slot.outpoint = COutPoint{funding_tx->GetHash(), 0};
    free_slot.purpose = PoolPurpose::OPERATIONAL;
    free_slot.asset = PoolAsset::DGB;
    free_slot.state = PoolEntryState::AVAILABLE;
    free_slot.script_pub_key = funding.vout.front().scriptPubKey;
    free_slot.dgb_value = DGBSatoshis{COIN / 10};
    free_slot.confirmation_height = height;
    free_slot.updated_at = GetTime();
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(
            free_slot.outpoint, Coin{funding.vout.front(), height, false}, false);
    }
    {
        LOCK(wallet.cs_wallet);
        BOOST_REQUIRE(wallet.AddToWallet(funding_tx, TxStateConfirmed{tip, height, 1}));
        WalletBatch batch{wallet.GetDatabase()};
        std::vector<ProviderPoolEntry> pool;
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(pool));
        pool.push_back(free_slot);
        BOOST_REQUIRE(batch.WritePaymasterProviderPool(pool));
        ProviderBudgetLedger ledger;
        BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(ledger));
        const uint256 bucket = GetNetgroupBudgetBucket(ledger, {1, 8, 8});
        const uint256 prior_key = SecurityTestId(seed, 701);
        const uint256 prior_hash = SecurityTestId(seed, 702);
        BOOST_REQUIRE_MESSAGE(ReserveProviderCapacityAdmission(ledger, environment.safety,
            prior_key, prior_hash, bucket, FundingModel::SPONSORED, false,
            GetTime() + 30, GetTime(), error), error);
        BOOST_REQUIRE_MESSAGE(ReleaseProviderCapacityAdmission(ledger, prior_key, prior_hash, GetTime(), error), error);
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(ledger));
        auto safety = environment.safety;
        safety.maximum_quote_requests_per_netgroup_per_minute = 1;
        BOOST_REQUIRE(batch.WritePaymasterProviderSafetyPolicy(safety));
    }
    BOOST_REQUIRE(paymaster_app::GetProviderReadiness(wallet, context).ready);
    {
        // A mixed offer retains DGB-only service when carriers are absent.
        // A budget available only to the carrier-dependent model must not
        // produce a false ready result after intersecting the model masks.
        auto mixed = environment.policy;
        mixed.funding_models = FUNDING_MODEL_ALL;
        auto settings = environment.settings;
        settings.policy_hash = GetProviderPolicyHash(mixed);
        ProviderSafetyPolicy original_safety;
        BOOST_REQUIRE(GetPaymasterProviderSafetyPolicy(wallet, original_safety));
        auto safety = original_safety;
        safety.user_paid = safety.public_sponsored;
        const auto save = [&] {
            LOCK(wallet.cs_wallet);
            WalletBatch batch{wallet.GetDatabase()};
            BOOST_REQUIRE(batch.WritePaymasterPolicy(mixed));
            BOOST_REQUIRE(batch.WritePaymasterSettings(settings));
            BOOST_REQUIRE(batch.WritePaymasterProviderSafetyPolicy(safety));
        };
        save();
        auto readiness = paymaster_app::GetProviderReadiness(wallet, context);
        BOOST_REQUIRE(readiness.ready);
        BOOST_CHECK_EQUAL(readiness.available_funding_models, FUNDING_MODEL_SPONSORED);
        Announcement announcement;
        BOOST_REQUIRE_MESSAGE(BuildPaymasterAnnouncement(wallet, environment.identity, mixed,
            readiness.pool_entries, LookupNumeric("127.0.0.1:18445"), Params().GenesisBlock().GetHash(),
            tip, GetTime(), readiness.available_funding_models, announcement, error), error);
        BOOST_REQUIRE_EQUAL(announcement.offers.size(), 1U);
        BOOST_CHECK(announcement.offers.front().funding_model == FundingModel::SPONSORED);
        safety.public_sponsored.maximum_reserved_network_fee = DGBSatoshis{COIN / 10};
        save();
        readiness = paymaster_app::GetProviderReadiness(wallet, context);
        BOOST_CHECK(!readiness.ready);
        BOOST_CHECK_EQUAL(readiness.available_funding_models, 0U);
        mixed.maximum_network_fee = DGBSatoshis{COIN / 10 - 1};
        settings.policy_hash = GetProviderPolicyHash(mixed);
        safety.public_sponsored.maximum_network_fee_per_transaction = mixed.maximum_network_fee;
        safety.user_paid.maximum_network_fee_per_transaction = mixed.maximum_network_fee;
        save();
        readiness = paymaster_app::GetProviderReadiness(wallet, context);
        BOOST_CHECK(!readiness.ready);
        BOOST_CHECK(std::find(readiness.errors.begin(), readiness.errors.end(), "PAYMASTER_NETWORK_FEE_CAP_TOO_LOW") != readiness.errors.end());
        BOOST_CHECK(!BuildPaymasterAnnouncement(wallet, environment.identity, mixed,
            readiness.pool_entries, LookupNumeric("127.0.0.1:18445"), Params().GenesisBlock().GetHash(),
            tip, GetTime(), FUNDING_MODEL_ALL, announcement, error));
        BOOST_CHECK_EQUAL(error, "PAYMASTER_NETWORK_FEE_CAP_TOO_LOW");
        mixed = environment.policy;
        settings = environment.settings;
        safety = original_safety;
        save();
    }
    PaymasterCapacityRequest capacity = fixture.capacity_request;
    capacity.request_id = "550e8400-e29b-41d4-a716-446655441221";
    capacity.session_id = SecurityTestId(seed, 703);
    capacity.client_nonce = SecurityTestId(seed, 704);
    capacity.created_at = GetTime();
    capacity.expires_at = GetTime() + 30;
    BOOST_REQUIRE_MESSAGE(ValidateCapacityRequestEnvelope(capacity, Params().GenesisBlock().GetHash(), GetTime(), error), error);
    const auto bytes = SerializePaymasterSecurityObject(capacity);
    BOOST_REQUIRE(manager.EnqueueNetworkDirectMessage(44, Hash(bytes), bytes.size(), DirectPayload{capacity},
        GetTime(), 44, {1, 8, 8}, DirectAdmissionClass::REQUEST) == DirectEnqueueResult::ACCEPTED);
    EnqueueQuote(manager, fixture.request);
    const auto before = GetMockableDatabase(wallet).m_records;
    const auto checkpoint_before = CheckpointBytes();
    const auto deferred = paymaster_app::ProcessPaymasterRequests(context, wallet, paymaster_app::ExecutionMode::MANUAL);
    BOOST_CHECK(deferred.HasMessage());
    BOOST_CHECK(deferred.IsDeferred());
    BOOST_CHECK_EQUAL(deferred.deferral_reason.value_or(""), "PAYMASTER_QUOTE_REQUEST_RATE_EXHAUSTED");
    BOOST_CHECK_EQUAL(deferred.message_type.value_or(""), "capacity");
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 2U);
    BOOST_CHECK_EQUAL(manager.OutboundDirectMessageCount(), 0U);
    BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);
    BOOST_CHECK(CheckpointBytes() == checkpoint_before);
    const auto continuation = paymaster_app::ProcessPaymasterRequests(context, wallet, paymaster_app::ExecutionMode::MANUAL);
    BOOST_CHECK_EQUAL(continuation.message_type.value_or(""), "quote");
    BOOST_CHECK(continuation.queued.value_or(false));
    BOOST_CHECK_EQUAL(manager.DirectMessageCount(), 1U);
    const auto retained = manager.LeaseCapacityRequests(environment.identity.provider_id, 1);
    BOOST_REQUIRE_EQUAL(retained.Messages().size(), 1U);
    BOOST_CHECK(GetMockableDatabase(wallet).m_records == before);
    BOOST_CHECK(CheckpointBytes() == checkpoint_before);
}

BOOST_AUTO_TEST_SUITE_END()
