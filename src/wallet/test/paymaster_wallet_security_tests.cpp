// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Adversarial wallet tests for unauthorized Paymaster asset flows. */

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <coins.h>
#include <hash.h>
#include <index/txindex.h>
#include <key.h>
#include <key_io.h>
#include <node/transaction.h>
#include <paymaster/protocol.h>
#include <paymaster/provider.h>
#include <paymaster/psbt.h>
#include <paymaster/wire.h>
#include <script/descriptor.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <streams.h>
#include <test/util/index.h>
#include <validation.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterpsbt.h>
#include <wallet/paymasterstore.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <string_view>
#include <thread>
#include <vector>

using namespace DigiDollar::Paymaster;
using namespace wallet;

namespace {

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
    FlatSigningProvider provider;
    std::unique_ptr<Descriptor> descriptor = Parse(
        "tr(" + EncodeSecret(key) + ")", provider, error,
        /*require_checksum=*/false);
    if (!descriptor) return false;
    WalletDescriptor wallet_descriptor{
        std::move(descriptor), 0, 0, 1, 0};
    LOCK(wallet.cs_wallet);
    wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    return wallet.AddWalletDescriptor(
        wallet_descriptor, provider, "", false);
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
                               const uint256& genesis_hash = {})
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
    intent.expires_at = now + 30;
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
    quote.expires_at = now + 30;
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

} // namespace

BOOST_FIXTURE_TEST_SUITE(paymaster_wallet_security_tests, WalletTestingSetup)

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

    auto restarted_database = DuplicateMockDatabase(m_wallet.GetDatabase());
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

    auto corrupt_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet corrupt_wallet{m_node.chain.get(),
                                   "quote-security-missing-budget",
                                   std::move(corrupt_database)};
    {
        LOCK(corrupt_wallet.cs_wallet);
        WalletBatch batch{corrupt_wallet.GetDatabase()};
        ProviderBudgetLedger corrupt_ledger;
        BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(corrupt_ledger));
        corrupt_ledger.reservations.clear();
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(corrupt_ledger));
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
    const uint256 user_psbt_hash{Hash(user_signed.user_signed_psbt)};

    // Preserve the exact pre-authorization state for the stale-policy branch.
    auto stale_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    auto stopped_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    auto advertised_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    auto mismatched_budget_database =
        DuplicateMockDatabase(m_wallet.GetDatabase());
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
        ProviderSafetyPolicy changed_safety;
        BOOST_REQUIRE(
            batch.ReadPaymasterProviderSafetyPolicy(changed_safety));
        ++changed_safety.maximum_active_quotes_total;
        changed_safety.updated_at = now + 4;
        BOOST_REQUIRE(
            batch.WritePaymasterProviderSafetyPolicy(changed_safety));
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
            batch.WritePaymasterProviderSafetyPolicy(changed_safety));
        BOOST_REQUIRE(
            batch.WritePaymasterProviderBudgetLedger(mismatched_ledger));
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
        ProviderSettings stopped_settings;
        BOOST_REQUIRE(batch.ReadPaymasterSettings(stopped_settings));
        stopped_settings.enabled = false;
        stopped_settings.updated_at = now + 4;
        BOOST_REQUIRE(batch.WritePaymasterSettings(stopped_settings));
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
        ProviderPolicy changed_policy;
        ProviderSettings changed_settings;
        BOOST_REQUIRE(batch.ReadPaymasterPolicy(changed_policy));
        BOOST_REQUIRE(batch.ReadPaymasterSettings(changed_settings));
        ++changed_policy.min_payment.value;
        changed_settings.policy_hash =
            GetProviderPolicyHash(changed_policy);
        changed_settings.updated_at = now + 4;
        BOOST_REQUIRE(batch.WritePaymasterPolicy(changed_policy));
        BOOST_REQUIRE(batch.WritePaymasterSettings(changed_settings));
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

    auto pool_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet pool_wallet{m_node.chain.get(),
                                "pre-signature-pool-conflict",
                                std::move(pool_database)};
    {
        LOCK(pool_wallet.cs_wallet);
        WalletBatch batch{pool_wallet.GetDatabase()};
        std::vector<ProviderPoolEntry> pool;
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(pool));
        BOOST_REQUIRE_EQUAL(pool.size(), 1U);
        pool.front().reservation_id = SecurityTestId(uint256S("1701"), 99);
        BOOST_REQUIRE(batch.WritePaymasterProviderPool(pool));
    }
    PaymasterStore pool_store{pool_wallet};
    BOOST_CHECK(!pool_store.ValidateProviderPreSignatureAuthorization(
        authorized, fixture.request.intent.genesis_hash, now + 4,
        capacity_request, capacity_proof, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_RESERVATION_CONFLICT");
    BOOST_CHECK(capacity_request.provider_id.IsNull());
    BOOST_CHECK(capacity_proof.provider_id.IsNull());

    auto budget_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet budget_wallet{m_node.chain.get(),
                                  "pre-signature-budget-conflict",
                                  std::move(budget_database)};
    {
        LOCK(budget_wallet.cs_wallet);
        WalletBatch batch{budget_wallet.GetDatabase()};
        ProviderBudgetLedger ledger;
        BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(ledger));
        const auto reservation = std::find_if(
            ledger.reservations.begin(), ledger.reservations.end(),
            [&](const ProviderBudgetReservation& entry) {
                return entry.commit_key == authorized.commit_key;
            });
        BOOST_REQUIRE(reservation != ledger.reservations.end());
        ++reservation->network_fee.value;
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(ledger));
    }
    PaymasterStore budget_store{budget_wallet};
    BOOST_CHECK(!budget_store.ValidateProviderPreSignatureAuthorization(
        authorized, fixture.request.intent.genesis_hash, now + 4,
        capacity_request, capacity_proof, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_PROVIDER_BUDGET_BINDING_MISMATCH");

    auto capacity_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet capacity_wallet{m_node.chain.get(),
                                    "pre-signature-capacity-missing",
                                    std::move(capacity_database)};
    {
        LOCK(capacity_wallet.cs_wallet);
        WalletBatch batch{capacity_wallet.GetDatabase()};
        BOOST_REQUIRE(batch.ErasePaymasterCapacityResponse(
            fixture.capacity_request_hash));
    }
    PaymasterStore capacity_store{capacity_wallet};
    BOOST_CHECK(!capacity_store.ValidateProviderPreSignatureAuthorization(
        authorized, fixture.request.intent.genesis_hash, now + 4,
        capacity_request, capacity_proof, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_RESERVATION_MISSING");
}

BOOST_AUTO_TEST_CASE(
    provider_recovery_pre_signature_firewall_reloads_pool_and_budget)
{
    constexpr int64_t now{1750};
    constexpr auto request_id =
        "550e8400-e29b-41d4-a716-446655441018";
    const uint256 seed{uint256S("1751")};
    const uint256 genesis_hash{SecurityTestId(seed, 1)};
    ProviderSecurityEnvironment environment =
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

    auto pool_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet pool_wallet{m_node.chain.get(),
                                "recovery-pre-signature-pool-conflict",
                                std::move(pool_database)};
    {
        LOCK(pool_wallet.cs_wallet);
        WalletBatch batch{pool_wallet.GetDatabase()};
        std::vector<ProviderPoolEntry> pool;
        BOOST_REQUIRE(batch.ReadPaymasterProviderPool(pool));
        BOOST_REQUIRE_EQUAL(pool.size(), 1U);
        pool.front().reservation_id = SecurityTestId(seed, 12);
        BOOST_REQUIRE(batch.WritePaymasterProviderPool(pool));
    }
    PaymasterStore pool_store{pool_wallet};
    BOOST_CHECK(!pool_store
                     .ValidateProviderAlternativeRecoveryPreSignatureAuthorization(
                         recovery, genesis_hash, now + 2, error));
    BOOST_CHECK_EQUAL(error, "PAYMASTER_CAPACITY_RESERVATION_CONFLICT");

    auto budget_database = DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet budget_wallet{m_node.chain.get(),
                                  "recovery-pre-signature-budget-conflict",
                                  std::move(budget_database)};
    {
        LOCK(budget_wallet.cs_wallet);
        WalletBatch batch{budget_wallet.GetDatabase()};
        ProviderBudgetLedger ledger;
        BOOST_REQUIRE(batch.ReadPaymasterProviderBudgetLedger(ledger));
        const auto reservation = std::find_if(
            ledger.reservations.begin(), ledger.reservations.end(),
            [&](const ProviderBudgetReservation& entry) {
                return entry.commit_key == response.recovery_commit_key;
            });
        BOOST_REQUIRE(reservation != ledger.reservations.end());
        ++reservation->network_fee.value;
        BOOST_REQUIRE(batch.WritePaymasterProviderBudgetLedger(ledger));
    }
    PaymasterStore budget_store{budget_wallet};
    BOOST_CHECK(!budget_store
                     .ValidateProviderAlternativeRecoveryPreSignatureAuthorization(
                         recovery, genesis_hash, now + 2, error));
    BOOST_CHECK_EQUAL(
        error, "PAYMASTER_PROVIDER_RECOVERY_BUDGET_BINDING_MISMATCH");
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
            auto database = DuplicateMockDatabase(m_wallet.GetDatabase());
            wallet::CWallet outdated_wallet{
                m_node.chain.get(), wallet_name, std::move(database)};
            BOOST_REQUIRE(outdated_wallet.LoadWallet() == DBErrors::LOAD_OK);
            {
                LOCK(outdated_wallet.cs_wallet);
                WalletBatch batch{outdated_wallet.GetDatabase()};
                ProviderAttempt persisted;
                BOOST_REQUIRE(batch.ReadPaymasterAttempt(
                    fixture.attempt.attempt_id, persisted));
                mutate(persisted, batch);
                BOOST_REQUIRE(batch.WritePaymasterAttempt(persisted));
            }
            PaymasterStore outdated_store{outdated_wallet};
            return outdated_store.ValidateClientDurableFinalForBroadcast(
                final, now + 2,
                /*exact_final_already_known=*/false, variant_error);
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
        BOOST_REQUIRE(batch.WritePaymasterAttempt(foreign_attempt));
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
        final, now + 2, /*exact_final_already_known=*/false, error));
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
                              transaction_output_value),
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
        DuplicateMockDatabase(m_wallet.GetDatabase());
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
            DuplicateMockDatabase(m_wallet.GetDatabase());
        wallet::CWallet tampered_wallet{
            m_node.chain.get(), wallet_name,
            std::move(tampered_database)};
        BOOST_REQUIRE(tampered_wallet.LoadWallet() == DBErrors::LOAD_OK);
        ProviderAttempt tampered;
        {
            LOCK(tampered_wallet.cs_wallet);
            WalletBatch batch{tampered_wallet.GetDatabase()};
            BOOST_REQUIRE(batch.ReadPaymasterAttempt(
                provider_signed.attempt_id, tampered));
            mutate(tampered);
            BOOST_REQUIRE(batch.WritePaymasterAttempt(tampered));
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
                    DuplicateMockDatabase(boundary_wallet.GetDatabase());
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
    const DurablePaymasterRecoveryReport recovery =
        RecoverDurablePaymasterCommits(
            m_wallet, commit.committed_at);
    BOOST_CHECK_EQUAL(recovery.candidates, 1U);
    BOOST_CHECK_EQUAL(recovery.recovered, 1U);
    BOOST_CHECK(recovery.errors.empty());
    BOOST_CHECK(m_wallet.IsLocked());
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

    auto restarted_database = DuplicateMockDatabase(m_wallet.GetDatabase());
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

    WITH_LOCK(m_node.mempool->cs,
              m_node.mempool->removeRecursive(
                  *final_ref, MemPoolRemovalReason::CONFLICT));
    {
        LOCK(::cs_main);
        auto& coins = m_node.chainman->ActiveChainstate().CoinsTip();
        for (const CTxIn& input : final_transaction.vin) {
            BOOST_REQUIRE(coins.SpendCoin(input.prevout));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
