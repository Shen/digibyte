// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Paymaster wallet persistence decoder and reference-integrity fuzzer.
 *
 * Arbitrary bytes are installed under authentic wallet database keys before
 * the record-specific decoder is called. A second property model mutates the
 * session, template, and unsigned-transaction indexes independently and
 * verifies that PaymasterStore never returns a mismatched referent or mutates
 * durable state while resolving it.
 */

#include <paymaster/protocol.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/util/setup_common.h>
#include <wallet/paymasterstore.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace DigiDollar::Paymaster;

namespace wallet {
namespace {

const TestingSetup* g_setup;

void InitializePaymasterPersistence()
{
    static const auto testing_setup =
        MakeNoLogFileContext<const TestingSetup>(ChainType::REGTEST);
    g_setup = testing_setup.get();
}

template <typename Key>
SerializeData SerializeDatabaseKey(const Key& key)
{
    DataStream stream;
    stream << key;
    return {stream.begin(), stream.end()};
}

template <typename Value>
SerializeData SerializeDatabaseValue(const Value& value)
{
    CDataStream stream{SER_DISK, CLIENT_VERSION};
    stream << value;
    return {stream.begin(), stream.end()};
}

template <typename Key>
void InstallRawRecord(MockableDatabase& database,
                      const Key& key,
                      const std::vector<uint8_t>& raw)
{
    const Span<const std::byte> bytes = AsBytes(Span{raw});
    database.m_records[SerializeDatabaseKey(key)] =
        SerializeData{bytes.begin(), bytes.end()};
}

template <typename Key>
void CorruptRecord(MockableDatabase& database, const Key& key)
{
    database.m_records[SerializeDatabaseKey(key)] =
        SerializeData{std::byte{0xff}};
}

template <typename Value, typename Key, typename Reader, typename Writer>
void ExerciseRecordDecoder(const Key& key,
                           const std::vector<uint8_t>& raw,
                           Reader&& reader,
                           Writer&& writer)
{
    MockableDatabase database;
    InstallRawRecord(database, key, raw);
    const MockableData before_read = database.m_records;

    Value decoded;
    DatabaseReadStatus status;
    {
        WalletBatch batch{database};
        status = reader(batch, decoded);
    }
    // The exact key is present. It may be malformed or from another version,
    // but it must never be confused with an absent record.
    assert(status != DatabaseReadStatus::NOT_FOUND);
    assert(database.m_records == before_read);
    if (status != DatabaseReadStatus::FOUND) return;

    const SerializeData canonical = SerializeDatabaseValue(decoded);
    const Span<const std::byte> raw_bytes = AsBytes(Span{raw});
    assert(canonical.size() <= raw_bytes.size());
    assert(std::equal(canonical.begin(), canonical.end(), raw_bytes.begin()));

    // Every accepted record must also pass the corresponding writer and be a
    // stable decode/encode fixed point in a clean database.
    MockableDatabase canonical_database;
    {
        WalletBatch batch{canonical_database};
        assert(writer(batch, decoded));
    }
    Value roundtrip;
    {
        WalletBatch batch{canonical_database};
        assert(reader(batch, roundtrip) == DatabaseReadStatus::FOUND);
    }
    assert(SerializeDatabaseValue(roundtrip) == canonical);
}

void ApplySessionReferenceMutation(MockableDatabase& database,
                                   PaymentSession session,
                                   uint8_t mutation)
{
    const auto session_key = std::make_pair(
        DBKeys::PAYMASTER_SESSION, session.request_id);
    const auto index_key = std::make_pair(
        DBKeys::PAYMASTER_SESSION_ID, session.session_id);
    switch (mutation) {
    case 0:
        return;
    case 1: {
        WalletBatch batch{database};
        assert(batch.WritePaymasterSessionId(
            session.session_id,
            "550e8400-e29b-41d4-a716-44665544a099"));
        return;
    }
    case 2: {
        WalletBatch batch{database};
        assert(batch.ErasePaymasterSession(session.request_id));
        return;
    }
    case 3:
        CorruptRecord(database, session_key);
        return;
    case 4: {
        session.session_id = uint256S("a0ff");
        WalletBatch batch{database};
        assert(batch.WritePaymasterSession(session));
        return;
    }
    case 5:
        CorruptRecord(database, index_key);
        return;
    case 6: {
        CDataStream truncated{SER_DISK, CLIENT_VERSION};
        truncated << PaymentSession::CURRENT_VERSION;
        database.m_records[SerializeDatabaseKey(session_key)] =
            SerializeData{truncated.begin(), truncated.end()};
        return;
    }
    }
    assert(false);
}

enum class AttemptReferenceKind {
    TEMPLATE,
    UNSIGNED_TRANSACTION,
};

void ApplyAttemptReferenceMutation(MockableDatabase& database,
                                   ProviderAttempt attempt,
                                   AttemptReferenceKind kind,
                                   uint8_t mutation)
{
    const auto attempt_key = std::make_pair(
        DBKeys::PAYMASTER_ATTEMPT, attempt.attempt_id);
    const auto reference_key =
        kind == AttemptReferenceKind::TEMPLATE ? std::make_pair(DBKeys::PAYMASTER_TEMPLATE,
                                                                attempt.template_commitment) :
                                                 std::make_pair(DBKeys::PAYMASTER_UNSIGNED_TX,
                                                                attempt.unsigned_txid);
    switch (mutation) {
    case 0:
        return;
    case 1: {
        WalletBatch batch{database};
        if (kind == AttemptReferenceKind::TEMPLATE) {
            assert(batch.WritePaymasterTemplate(
                attempt.template_commitment, uint256S("a1ff"), true));
        } else {
            assert(batch.WritePaymasterUnsignedTx(
                attempt.unsigned_txid, uint256S("a2ff"), true));
        }
        return;
    }
    case 2: {
        WalletBatch batch{database};
        assert(batch.ErasePaymasterAttempt(attempt.attempt_id));
        return;
    }
    case 3:
        CorruptRecord(database, attempt_key);
        return;
    case 4: {
        if (kind == AttemptReferenceKind::TEMPLATE) {
            attempt.template_commitment = uint256S("a3ff");
        } else {
            attempt.unsigned_txid = uint256S("a4ff");
        }
        WalletBatch batch{database};
        assert(batch.WritePaymasterAttempt(attempt));
        return;
    }
    case 5:
        CorruptRecord(database, reference_key);
        return;
    case 6: {
        CDataStream truncated{SER_DISK, CLIENT_VERSION};
        truncated << ProviderAttempt::CURRENT_VERSION;
        database.m_records[SerializeDatabaseKey(attempt_key)] =
            SerializeData{truncated.begin(), truncated.end()};
        return;
    }
    }
    assert(false);
}

void ExerciseReferenceIntegrity(uint8_t session_mutation,
                                uint8_t template_mutation,
                                uint8_t unsigned_mutation)
{
    auto database = std::make_unique<MockableDatabase>();

    PaymentSession session;
    session.request_id = "550e8400-e29b-41d4-a716-44665544a001";
    session.session_id = uint256S("a001");
    session.canonical_request_hash = uint256S("a002");
    session.created_at = 1;
    session.updated_at = 1;

    ProviderAttempt template_attempt;
    template_attempt.session_id = session.session_id;
    template_attempt.attempt_id = uint256S("a101");
    template_attempt.template_commitment = uint256S("a102");
    template_attempt.unsigned_txid = uint256S("a103");
    template_attempt.created_at = 1;
    template_attempt.updated_at = 1;

    ProviderAttempt unsigned_attempt;
    unsigned_attempt.session_id = session.session_id;
    unsigned_attempt.attempt_id = uint256S("a201");
    unsigned_attempt.template_commitment = uint256S("a202");
    unsigned_attempt.unsigned_txid = uint256S("a203");
    unsigned_attempt.created_at = 1;
    unsigned_attempt.updated_at = 1;

    {
        WalletBatch batch{*database};
        assert(batch.WritePaymasterSession(session));
        assert(batch.WritePaymasterSessionId(
            session.session_id, session.request_id));
        assert(batch.WritePaymasterAttempt(template_attempt));
        assert(batch.WritePaymasterTemplate(
            template_attempt.template_commitment,
            template_attempt.attempt_id));
        assert(batch.WritePaymasterAttempt(unsigned_attempt));
        assert(batch.WritePaymasterUnsignedTx(
            unsigned_attempt.unsigned_txid,
            unsigned_attempt.attempt_id));
    }

    ApplySessionReferenceMutation(
        *database, session, session_mutation);
    ApplyAttemptReferenceMutation(
        *database, template_attempt, AttemptReferenceKind::TEMPLATE,
        template_mutation);
    ApplyAttemptReferenceMutation(
        *database, unsigned_attempt,
        AttemptReferenceKind::UNSIGNED_TRANSACTION, unsigned_mutation);

    CWallet wallet{g_setup->m_node.chain.get(),
                   "paymaster-persistence-fuzz", std::move(database)};
    PaymasterStore store{wallet};
    const MockableData before_lookup = GetMockableDatabase(wallet).m_records;

    PaymentSession resolved_session;
    const bool found_session = store.GetSessionBySessionId(
        session.session_id, resolved_session);
    assert(found_session == (session_mutation == 0));
    if (found_session) {
        assert(resolved_session.request_id == session.request_id);
        assert(resolved_session.session_id == session.session_id);
    }

    ProviderAttempt resolved_attempt;
    const bool found_template = store.GetAttemptByTemplateCommitment(
        template_attempt.template_commitment, resolved_attempt);
    assert(found_template == (template_mutation == 0));
    if (found_template) {
        assert(resolved_attempt.attempt_id == template_attempt.attempt_id);
        assert(resolved_attempt.template_commitment ==
               template_attempt.template_commitment);
    }

    resolved_attempt = {};
    const bool found_unsigned = store.GetAttemptByUnsignedTxid(
        unsigned_attempt.unsigned_txid, resolved_attempt);
    assert(found_unsigned == (unsigned_mutation == 0));
    if (found_unsigned) {
        assert(resolved_attempt.attempt_id == unsigned_attempt.attempt_id);
        assert(resolved_attempt.unsigned_txid ==
               unsigned_attempt.unsigned_txid);
    }
    assert(GetMockableDatabase(wallet).m_records == before_lookup);
}

} // namespace

FUZZ_TARGET(paymaster_persistence_records,
            .init = InitializePaymasterPersistence)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};
    const uint8_t record_kind =
        provider.ConsumeIntegralInRange<uint8_t>(0, 34);
    const uint8_t session_mutation =
        provider.ConsumeIntegralInRange<uint8_t>(0, 6);
    const uint8_t template_mutation =
        provider.ConsumeIntegralInRange<uint8_t>(0, 6);
    const uint8_t unsigned_mutation =
        provider.ConsumeIntegralInRange<uint8_t>(0, 6);
    const std::vector<uint8_t> raw =
        provider.ConsumeRemainingBytes<uint8_t>();

    ExerciseReferenceIntegrity(
        session_mutation, template_mutation, unsigned_mutation);

    constexpr auto request_id =
        "550e8400-e29b-41d4-a716-44665544b001";
    const uint256 id = uint256S("b001");
    const PaymasterId provider_id{uint256S("b002")};
    const COutPoint outpoint{uint256S("b003"), 1};

    switch (record_kind) {
    case 0:
        ExerciseRecordDecoder<PaymentSession>(
            std::make_pair(DBKeys::PAYMASTER_SESSION,
                           std::string{request_id}),
            raw,
            [&](WalletBatch& batch, PaymentSession& value) {
                return batch.ReadPaymasterSessionWithStatus(
                    request_id, value);
            },
            [](WalletBatch& batch, const PaymentSession& value) {
                return batch.WritePaymasterSession(value);
            });
        break;
    case 1:
        ExerciseRecordDecoder<SelfRecoveryRecord>(
            std::make_pair(DBKeys::PAYMASTER_RECOVERY,
                           std::string{request_id}),
            raw,
            [&](WalletBatch& batch, SelfRecoveryRecord& value) {
                return batch.ReadPaymasterRecoveryWithStatus(
                    request_id, value);
            },
            [](WalletBatch& batch, const SelfRecoveryRecord& value) {
                return batch.WritePaymasterRecovery(value);
            });
        break;
    case 2:
        ExerciseRecordDecoder<AlternativeRecoveryRecord>(
            std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY, id), raw,
            [&](WalletBatch& batch, AlternativeRecoveryRecord& value) {
                return batch.ReadPaymasterAlternativeRecoveryWithStatus(
                    id, value);
            },
            [](WalletBatch& batch,
               const AlternativeRecoveryRecord& value) {
                return batch.WritePaymasterAlternativeRecovery(value);
            });
        break;
    case 3:
        ExerciseRecordDecoder<uint256>(
            std::make_pair(DBKeys::PAYMASTER_ALT_RECOVERY_REQUEST,
                           std::string{request_id}),
            raw,
            [&](WalletBatch& batch, uint256& value) {
                return batch.ReadPaymasterAlternativeRecoveryRequestWithStatus(
                    request_id, value);
            },
            [&](WalletBatch& batch, const uint256& value) {
                return batch.WritePaymasterAlternativeRecoveryRequest(
                    request_id, value);
            });
        break;
    case 4:
        ExerciseRecordDecoder<ProviderAttempt>(
            std::make_pair(DBKeys::PAYMASTER_ATTEMPT, id), raw,
            [&](WalletBatch& batch, ProviderAttempt& value) {
                return batch.ReadPaymasterAttemptWithStatus(id, value);
            },
            [](WalletBatch& batch, const ProviderAttempt& value) {
                return batch.WritePaymasterAttempt(value);
            });
        break;
    case 5:
        ExerciseRecordDecoder<ValidatedCapacitySnapshot>(
            std::make_pair(DBKeys::PAYMASTER_CAPACITY, id), raw,
            [&](WalletBatch& batch, ValidatedCapacitySnapshot& value) {
                return batch.ReadPaymasterCapacitySnapshotWithStatus(
                    id, value);
            },
            [](WalletBatch& batch,
               const ValidatedCapacitySnapshot& value) {
                return batch.WritePaymasterCapacitySnapshot(value);
            });
        break;
    case 6:
        ExerciseRecordDecoder<CapacityResourceBinding>(
            std::make_pair(
                DBKeys::PAYMASTER_CAPACITY_RESOURCE,
                std::make_pair(provider_id, outpoint)),
            raw,
            [&](WalletBatch& batch, CapacityResourceBinding& value) {
                return batch.ReadPaymasterCapacityResourceWithStatus(
                    provider_id, outpoint, value);
            },
            [](WalletBatch& batch,
               const CapacityResourceBinding& value) {
                return batch.WritePaymasterCapacityResource(value);
            });
        break;
    case 7:
        ExerciseRecordDecoder<ProviderCapacityReleaseRecord>(
            std::make_pair(DBKeys::PAYMASTER_CAPACITY_RELEASE, id), raw,
            [&](WalletBatch& batch,
                ProviderCapacityReleaseRecord& value) {
                return batch.ReadPaymasterCapacityReleaseWithStatus(
                    id, value);
            },
            [](WalletBatch& batch,
               const ProviderCapacityReleaseRecord& value) {
                return batch.WritePaymasterCapacityRelease(value);
            });
        break;
    case 8:
        ExerciseRecordDecoder<uint256>(
            std::make_pair(DBKeys::PAYMASTER_TEMPLATE, id), raw,
            [&](WalletBatch& batch, uint256& value) {
                return batch.ReadPaymasterTemplateWithStatus(id, value);
            },
            [&](WalletBatch& batch, const uint256& value) {
                return batch.WritePaymasterTemplate(id, value);
            });
        break;
    case 9:
        ExerciseRecordDecoder<uint256>(
            std::make_pair(DBKeys::PAYMASTER_UNSIGNED_TX, id), raw,
            [&](WalletBatch& batch, uint256& value) {
                return batch.ReadPaymasterUnsignedTxWithStatus(id, value);
            },
            [&](WalletBatch& batch, const uint256& value) {
                return batch.WritePaymasterUnsignedTx(id, value);
            });
        break;
    case 10:
        ExerciseRecordDecoder<std::string>(
            std::make_pair(DBKeys::PAYMASTER_SESSION_ID, id), raw,
            [&](WalletBatch& batch, std::string& value) {
                return batch.ReadPaymasterSessionIdWithStatus(id, value);
            },
            [&](WalletBatch& batch, const std::string& value) {
                return batch.WritePaymasterSessionId(id, value);
            });
        break;
    case 11:
        ExerciseRecordDecoder<InputReservation>(
            std::make_pair(DBKeys::PAYMASTER_RESERVATION, outpoint), raw,
            [&](WalletBatch& batch, InputReservation& value) {
                return batch.ReadPaymasterReservationWithStatus(
                    outpoint, value);
            },
            [](WalletBatch& batch, const InputReservation& value) {
                return batch.WritePaymasterReservation(value);
            });
        break;
    case 12:
        ExerciseRecordDecoder<IdempotencyTombstone>(
            std::make_pair(DBKeys::PAYMASTER_TOMBSTONE,
                           std::string{request_id}),
            raw,
            [&](WalletBatch& batch, IdempotencyTombstone& value) {
                return batch.ReadPaymasterTombstoneWithStatus(
                    request_id, value);
            },
            [](WalletBatch& batch,
               const IdempotencyTombstone& value) {
                return batch.WritePaymasterTombstone(value);
            });
        break;
    case 13:
        ExerciseRecordDecoder<ProviderCommitRecord>(
            std::make_pair(DBKeys::PAYMASTER_PROVIDER_COMMIT, id), raw,
            [&](WalletBatch& batch, ProviderCommitRecord& value) {
                return batch.ReadPaymasterProviderCommitWithStatus(
                    id, value);
            },
            [](WalletBatch& batch, const ProviderCommitRecord& value) {
                return batch.WritePaymasterProviderCommit(value);
            });
        break;
    case 14:
        ExerciseRecordDecoder<UserAuthorizationRecord>(
            std::make_pair(DBKeys::PAYMASTER_USER_AUTH, id), raw,
            [&](WalletBatch& batch, UserAuthorizationRecord& value) {
                return batch.ReadPaymasterUserAuthorizationWithStatus(
                    id, value);
            },
            [](WalletBatch& batch,
               const UserAuthorizationRecord& value) {
                return batch.WritePaymasterUserAuthorization(value);
            });
        break;
    case 15:
        ExerciseRecordDecoder<ProviderIdentityRecord>(
            DBKeys::PAYMASTER_IDENTITY, raw,
            [](WalletBatch& batch, ProviderIdentityRecord& value) {
                return batch.ReadPaymasterIdentityWithStatus(value);
            },
            [](WalletBatch& batch, const ProviderIdentityRecord& value) {
                return batch.WritePaymasterIdentity(value);
            });
        break;
    case 16:
        ExerciseRecordDecoder<ProviderPolicy>(
            DBKeys::PAYMASTER_POLICY, raw,
            [](WalletBatch& batch, ProviderPolicy& value) {
                return batch.ReadPaymasterPolicyWithStatus(value);
            },
            [](WalletBatch& batch, const ProviderPolicy& value) {
                return batch.WritePaymasterPolicy(value);
            });
        break;
    case 17:
        ExerciseRecordDecoder<ProviderSettings>(
            DBKeys::PAYMASTER_SETTINGS, raw,
            [](WalletBatch& batch, ProviderSettings& value) {
                return batch.ReadPaymasterSettingsWithStatus(value);
            },
            [](WalletBatch& batch, const ProviderSettings& value) {
                return batch.WritePaymasterSettings(value);
            });
        break;
    case 18:
        ExerciseRecordDecoder<ProviderSafetyPolicy>(
            DBKeys::PAYMASTER_PROVIDER_SAFETY, raw,
            [](WalletBatch& batch, ProviderSafetyPolicy& value) {
                return batch.ReadPaymasterProviderSafetyPolicyWithStatus(
                    value);
            },
            [](WalletBatch& batch, const ProviderSafetyPolicy& value) {
                return batch.WritePaymasterProviderSafetyPolicy(value);
            });
        break;
    case 19:
        ExerciseRecordDecoder<ClientSafetyPolicy>(
            DBKeys::PAYMASTER_CLIENT_SAFETY, raw,
            [](WalletBatch& batch, ClientSafetyPolicy& value) {
                return batch.ReadPaymasterClientSafetyPolicyWithStatus(
                    value);
            },
            [](WalletBatch& batch, const ClientSafetyPolicy& value) {
                return batch.WritePaymasterClientSafetyPolicy(value);
            });
        break;
    case 20:
        ExerciseRecordDecoder<ProviderBudgetLedger>(
            DBKeys::PAYMASTER_PROVIDER_BUDGET, raw,
            [](WalletBatch& batch, ProviderBudgetLedger& value) {
                return batch.ReadPaymasterProviderBudgetLedgerWithStatus(
                    value);
            },
            [](WalletBatch& batch, const ProviderBudgetLedger& value) {
                return batch.WritePaymasterProviderBudgetLedger(value);
            });
        break;
    case 21:
        ExerciseRecordDecoder<ClientFeeLedger>(
            DBKeys::PAYMASTER_CLIENT_FEES, raw,
            [](WalletBatch& batch, ClientFeeLedger& value) {
                return batch.ReadPaymasterClientFeeLedgerWithStatus(value);
            },
            [](WalletBatch& batch, const ClientFeeLedger& value) {
                return batch.WritePaymasterClientFeeLedger(value);
            });
        break;
    case 22:
        ExerciseRecordDecoder<SponsorshipAuthorizationRecord>(
            std::make_pair(DBKeys::PAYMASTER_SPONSOR_AUTH, id), raw,
            [&](WalletBatch& batch,
                SponsorshipAuthorizationRecord& value) {
                return batch
                    .ReadPaymasterSponsorshipAuthorizationWithStatus(
                        id, value);
            },
            [](WalletBatch& batch,
               const SponsorshipAuthorizationRecord& value) {
                return batch.WritePaymasterSponsorshipAuthorization(
                    value);
            });
        break;
    case 23:
        ExerciseRecordDecoder<std::vector<ProviderPoolEntry>>(
            DBKeys::PAYMASTER_PROVIDER_POOL, raw,
            [](WalletBatch& batch,
               std::vector<ProviderPoolEntry>& value) {
                return batch.ReadPaymasterProviderPoolWithStatus(value);
            },
            [](WalletBatch& batch,
               const std::vector<ProviderPoolEntry>& value) {
                return batch.WritePaymasterProviderPool(value);
            });
        break;
    case 24:
        ExerciseRecordDecoder<ProviderLiquidityPolicy>(
            DBKeys::PAYMASTER_LIQUIDITY_POLICY, raw,
            [](WalletBatch& batch, ProviderLiquidityPolicy& value) {
                return batch.ReadPaymasterLiquidityPolicyWithStatus(value);
            },
            [](WalletBatch& batch,
               const ProviderLiquidityPolicy& value) {
                return batch.WritePaymasterLiquidityPolicy(value);
            });
        break;
    case 25:
        ExerciseRecordDecoder<ProviderMaintenanceLedger>(
            DBKeys::PAYMASTER_MAINTENANCE_LEDGER, raw,
            [](WalletBatch& batch, ProviderMaintenanceLedger& value) {
                return batch.ReadPaymasterMaintenanceLedgerWithStatus(
                    value);
            },
            [](WalletBatch& batch,
               const ProviderMaintenanceLedger& value) {
                return batch.WritePaymasterMaintenanceLedger(value);
            });
        break;
    case 26:
        ExerciseRecordDecoder<ProviderFinanceLedger>(
            std::string{"pmfinance"}, raw,
            [](WalletBatch& batch, ProviderFinanceLedger& value) {
                return batch.ReadPaymasterFinanceLedgerWithStatus(value);
            },
            [](WalletBatch& batch,
               const ProviderFinanceLedger& value) {
                return batch.WritePaymasterFinanceLedger(value);
            });
        break;
    case 27:
        ExerciseRecordDecoder<ProviderBackupStatus>(
            std::string{"pmbackup"}, raw,
            [](WalletBatch& batch, ProviderBackupStatus& value) {
                return batch.ReadPaymasterBackupStatusWithStatus(value);
            },
            [](WalletBatch& batch, const ProviderBackupStatus& value) {
                return batch.WritePaymasterBackupStatus(value);
            });
        break;
    case 28:
        ExerciseRecordDecoder<ProviderCarrierWithdrawalPlan>(
            DBKeys::PAYMASTER_CARRIER_WITHDRAWAL, raw,
            [](WalletBatch& batch,
               ProviderCarrierWithdrawalPlan& value) {
                return batch
                    .ReadPaymasterCarrierWithdrawalPlanWithStatus(value);
            },
            [](WalletBatch& batch,
               const ProviderCarrierWithdrawalPlan& value) {
                return batch.WritePaymasterCarrierWithdrawalPlan(value);
            });
        break;
    case 29:
        ExerciseRecordDecoder<PaymasterResult>(
            std::make_pair(DBKeys::PAYMASTER_RESULT, id), raw,
            [&](WalletBatch& batch, PaymasterResult& value) {
                return batch.ReadPaymasterResultWithStatus(id, value);
            },
            [](WalletBatch& batch, const PaymasterResult& value) {
                return batch.WritePaymasterResult(value);
            });
        break;
    case 30:
        ExerciseRecordDecoder<PaymasterReliabilityRecord>(
            std::make_pair(DBKeys::PAYMASTER_RELIABILITY, provider_id),
            raw,
            [&](WalletBatch& batch, PaymasterReliabilityRecord& value) {
                return batch.ReadPaymasterReliabilityWithStatus(
                    provider_id, value);
            },
            [](WalletBatch& batch,
               const PaymasterReliabilityRecord& value) {
                return batch.WritePaymasterReliability(value);
            });
        break;
    case 31:
        ExerciseRecordDecoder<PaymasterEquivocationEvidence>(
            std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION, id), raw,
            [&](WalletBatch& batch,
                PaymasterEquivocationEvidence& value) {
                return batch.ReadPaymasterEquivocationEvidence(id, value);
            },
            [](WalletBatch& batch,
               const PaymasterEquivocationEvidence& value) {
                return batch.WritePaymasterEquivocationEvidence(value);
            });
        break;
    case 32:
        ExerciseRecordDecoder<PaymasterEquivocationEvidence>(
            std::make_pair(DBKeys::PAYMASTER_EQUIVOCATION_PENDING,
                           provider_id),
            raw,
            [&](WalletBatch& batch,
                PaymasterEquivocationEvidence& value) {
                return batch.ReadPaymasterPendingEquivocation(
                    provider_id, value);
            },
            [](WalletBatch& batch,
               const PaymasterEquivocationEvidence& value) {
                return batch.WritePaymasterPendingEquivocation(value);
            });
        break;
    case 33:
        ExerciseRecordDecoder<PaymasterProviderBlock>(
            std::make_pair(DBKeys::PAYMASTER_PROVIDER_BLOCK, provider_id),
            raw,
            [&](WalletBatch& batch, PaymasterProviderBlock& value) {
                return batch.ReadPaymasterProviderBlock(
                    provider_id, value);
            },
            [](WalletBatch& batch, const PaymasterProviderBlock& value) {
                return batch.WritePaymasterProviderBlock(value);
            });
        break;
    case 34:
        ExerciseRecordDecoder<PaymasterOutcomeMarker>(
            std::make_pair(DBKeys::PAYMASTER_OUTCOME, id), raw,
            [&](WalletBatch& batch, PaymasterOutcomeMarker& value) {
                return batch.ReadPaymasterOutcomeMarkerWithStatus(
                    id, value);
            },
            [](WalletBatch& batch, const PaymasterOutcomeMarker& value) {
                return batch.WritePaymasterOutcomeMarker(value);
            });
        break;
    }
}

} // namespace wallet
