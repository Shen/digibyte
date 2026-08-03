// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Stateful fuzzer for provider pool, maintenance, and withdrawal invariants.
 *
 * This target intentionally stays below wallet I/O.  Functional tests cover
 * RPC persistence and chain callbacks; here arbitrary operation ordering must
 * never produce an invalid durable shape, duplicate budget record, or a plan
 * that ceases to bind its exact sources and outputs.
 */

#include <hash.h>
#include <key.h>
#include <paymaster/provider.h>
#include <script/standard.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <tinyformat.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace DigiDollar::Paymaster;

namespace {

void initialize_paymaster_pool_lifecycle()
{
    ECC_Start();
}

CScript TestScript(uint8_t discriminator)
{
    std::array<unsigned char, 32> secret{};
    secret.back() = discriminator == 0 ? 1 : discriminator;
    CKey key;
    key.Set(secret.begin(), secret.end(), true);
    assert(key.IsValid());
    return GetScriptForDestination(
        WitnessV1Taproot{XOnlyPubKey{key.GetPubKey()}});
}

ProviderPoolEntry PoolEntry(uint32_t index, PoolPurpose purpose,
                            PoolAsset asset, int64_t value)
{
    ProviderPoolEntry entry;
    entry.outpoint = COutPoint{uint256::ONE, index};
    entry.purpose = purpose;
    entry.asset = asset;
    entry.script_pub_key = TestScript(static_cast<uint8_t>(index + 1));
    if (asset == PoolAsset::DGB) {
        entry.dgb_value = DGBSatoshis{value};
    } else {
        entry.carrier_value = DDCents{value};
    }
    entry.confirmation_height = 100;
    entry.updated_at = 1;
    return entry;
}

ProviderLiquidityPolicy LiquidityPolicy()
{
    ProviderLiquidityPolicy policy;
    policy.automatic_replenishment = true;
    policy.paid_maintenance_approved = true;
    policy.target_admission_dgb = 3;
    policy.target_operational_dgb = 1;
    policy.target_admission_carriers = 3;
    policy.target_operational_carriers = 1;
    policy.maximum_maintenance_fee_per_transaction = DGBSatoshis{100};
    policy.maximum_maintenance_fee_per_hour = DGBSatoshis{300};
    policy.maximum_maintenance_fee_per_day = DGBSatoshis{1000};
    policy.updated_at = 1;
    return policy;
}

ProviderMaintenanceOutput MaintenanceOutput(uint32_t index)
{
    ProviderMaintenanceOutput output;
    output.purpose = PoolPurpose::OPERATIONAL;
    output.asset = PoolAsset::DGB;
    output.script_pub_key = TestScript(static_cast<uint8_t>(index + 32));
    output.dgb_value = DGBSatoshis{200};
    return output;
}

template <typename T>
uint256 ObjectHash(const T& object)
{
    CDataStream stream{SER_NETWORK, ::PROTOCOL_VERSION};
    stream << object;
    return Hash(MakeUCharSpan(stream));
}

void AssertPoolInvariants(const std::vector<ProviderPoolEntry>& pool)
{
    std::string error;
    assert(ValidateProviderPoolEntries(pool, error));
    std::set<COutPoint> outpoints;
    for (const ProviderPoolEntry& entry : pool) {
        assert(outpoints.insert(entry.outpoint).second);
        if (entry.state == PoolEntryState::AVAILABLE ||
            entry.state == PoolEntryState::RELEASED ||
            entry.state == PoolEntryState::INVALIDATED ||
            entry.state == PoolEntryState::SPENT) {
            assert(entry.reservation_id.IsNull());
        }
        if (entry.state == PoolEntryState::PENDING_SUCCESSOR) {
            assert(!entry.origin_commit_key.IsNull());
            assert(entry.reservation_id == entry.origin_commit_key);
        }
    }
}

bool CountsTowardTarget(PoolEntryState state)
{
    return state == PoolEntryState::AVAILABLE ||
           state == PoolEntryState::RESERVED ||
           state == PoolEntryState::COMMITTED ||
           state == PoolEntryState::PENDING_SUCCESSOR;
}

uint32_t TargetFor(const ProviderLiquidityPolicy& policy,
                   PoolPurpose purpose, PoolAsset asset)
{
    if (asset == PoolAsset::DGB) {
        return purpose == PoolPurpose::ADMISSION
            ? policy.target_admission_dgb
            : policy.target_operational_dgb;
    }
    return purpose == PoolPurpose::ADMISSION
        ? policy.target_admission_carriers
        : policy.target_operational_carriers;
}

size_t ActiveCount(const std::vector<ProviderPoolEntry>& pool,
                   PoolPurpose purpose, PoolAsset asset)
{
    return std::count_if(pool.begin(), pool.end(),
                         [purpose, asset](const ProviderPoolEntry& entry) {
        return entry.purpose == purpose && entry.asset == asset &&
               CountsTowardTarget(entry.state);
    });
}

} // namespace

FUZZ_TARGET(paymaster_pool_lifecycle, .init = initialize_paymaster_pool_lifecycle)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};
    std::vector<ProviderPoolEntry> pool{
        PoolEntry(0, PoolPurpose::ADMISSION, PoolAsset::DGB, 10000000),
        PoolEntry(1, PoolPurpose::ADMISSION, PoolAsset::DGB, 10000000),
        PoolEntry(2, PoolPurpose::ADMISSION, PoolAsset::DGB, 10000000),
        PoolEntry(3, PoolPurpose::OPERATIONAL, PoolAsset::DGB, 10000000),
        PoolEntry(4, PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER, 100),
        PoolEntry(5, PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER, 100),
        PoolEntry(6, PoolPurpose::ADMISSION, PoolAsset::DD_CARRIER, 100),
        PoolEntry(7, PoolPurpose::OPERATIONAL, PoolAsset::DD_CARRIER, 100),
    };
    ProviderMaintenanceLedger ledger;
    const ProviderLiquidityPolicy policy{LiquidityPolicy()};
    uint32_t nonce{100};
    int64_t now{1000};

    while (provider.remaining_bytes() > 0) {
        const uint8_t command = provider.ConsumeIntegralInRange<uint8_t>(0, 14);
        const size_t index = provider.ConsumeIntegralInRange<size_t>(
            0, pool.size() - 1);
        ProviderPoolEntry& entry = pool[index];
        const uint256 operation_id{uint256S(strprintf("%x", ++nonce))};
        now = SaturatingAddSeconds(
            now, provider.ConsumeIntegralInRange<int64_t>(0, 3600));

        if (command == 0 && entry.state == PoolEntryState::AVAILABLE) {
            entry.state = PoolEntryState::RESERVED;
            entry.reservation_id = operation_id;
        } else if (command == 1 &&
                   entry.state == PoolEntryState::RESERVED) {
            entry.state = PoolEntryState::AVAILABLE;
            entry.reservation_id.SetNull();
        } else if (command == 2 &&
                   entry.state == PoolEntryState::RESERVED) {
            entry.state = PoolEntryState::COMMITTED;
        } else if (command == 3 &&
                   entry.state == PoolEntryState::COMMITTED) {
            entry.state = PoolEntryState::SPENT;
            entry.reservation_id.SetNull();
        } else if (command == 4) {
            ProviderMaintenanceRecord record;
            const bool replay = !ledger.records.empty() && provider.ConsumeBool();
            if (replay) {
                record = ledger.records[provider.ConsumeIntegralInRange<size_t>(
                    0, ledger.records.size() - 1)];
            } else {
                record.operation_id = operation_id;
                record.plan_id = ObjectHash(pool);
                record.outputs = {MaintenanceOutput(nonce)};
                record.maximum_fee = DGBSatoshis{
                    provider.ConsumeIntegralInRange<int64_t>(1, 100)};
            }
            std::string error;
            const uint256 before{ObjectHash(ledger)};
            const bool reserved = ReserveProviderMaintenanceBudget(
                ledger, policy, record, now, error);
            if (!reserved) assert(ObjectHash(ledger) == before);
            if (replay) {
                // An exact maintenance retry may be accepted idempotently or
                // rejected as already known, but it must never append a second
                // budget record or charge the fee again.
                assert(ObjectHash(ledger) == before);
            }
        } else if (command == 5 && !ledger.records.empty()) {
            const auto& record = ledger.records[
                provider.ConsumeIntegralInRange<size_t>(
                    0, ledger.records.size() - 1)];
            std::string error;
            ReleaseProviderMaintenanceBudget(
                ledger, record.operation_id, now, error);
        } else if (command == 6) {
            ProviderCarrierWithdrawalPlan plan;
            plan.plan_id = operation_id;
            plan.operation_id = ObjectHash(pool);
            plan.mode = CarrierWithdrawalMode::RELEASE_SLOT;
            plan.source_carriers = {pool[7].outpoint};
            plan.target_operational_carriers_after_release = 0;
            plan.liquidity_policy_updated_at = policy.updated_at;
            plan.created_at = now;
            plan.expires_at = SaturatingAddSeconds(now, 60);
            std::string error;
            assert(ValidateProviderCarrierWithdrawalPlan(plan, error));
            const uint256 valid_hash{ObjectHash(plan)};
            if (provider.ConsumeBool()) {
                plan.source_carriers.push_back(plan.source_carriers.front());
                assert(!ValidateProviderCarrierWithdrawalPlan(plan, error));
                assert(ObjectHash(plan) != valid_hash);
            }
        } else if (command == 7) {
            // Simulate restart persistence after any operation ordering.
            CDataStream stream{SER_DISK, ::PROTOCOL_VERSION};
            stream << pool << ledger;
            std::vector<ProviderPoolEntry> decoded_pool;
            ProviderMaintenanceLedger decoded_ledger;
            stream >> decoded_pool >> decoded_ledger;
            assert(ObjectHash(decoded_pool) == ObjectHash(pool));
            assert(ObjectHash(decoded_ledger) == ObjectHash(ledger));
            pool = std::move(decoded_pool);
            ledger = std::move(decoded_ledger);
        } else if (command == 8 && entry.state == PoolEntryState::SPENT &&
                   pool.size() < 64) {
            // A final provider commit can replace a spent slot with one
            // unconfirmed wallet-owned successor. Pending successors count
            // toward the target so scheduler replays cannot over-create them.
            const PoolPurpose purpose{entry.purpose};
            const PoolAsset asset{entry.asset};
            if (ActiveCount(pool, purpose, asset) <
                TargetFor(policy, purpose, asset)) {
                const int64_t value = asset == PoolAsset::DGB
                    ? entry.dgb_value.value
                    : entry.carrier_value.value;
                ProviderPoolEntry successor{
                    PoolEntry(++nonce, purpose, asset, value)};
                successor.state = PoolEntryState::PENDING_SUCCESSOR;
                successor.confirmation_height = 0;
                successor.origin_commit_key = operation_id;
                successor.reservation_id = operation_id;
                pool.push_back(std::move(successor));
            }
        } else if (command == 9 &&
                   entry.state == PoolEntryState::PENDING_SUCCESSOR) {
            entry.state = PoolEntryState::AVAILABLE;
            entry.confirmation_height = 101;
            entry.reservation_id.SetNull();
        } else if (command == 10 &&
                   entry.state == PoolEntryState::AVAILABLE &&
                   !entry.origin_commit_key.IsNull()) {
            // Reorgs remove readiness without forgetting the creating commit.
            entry.state = PoolEntryState::PENDING_SUCCESSOR;
            entry.confirmation_height = 0;
            entry.reservation_id = entry.origin_commit_key;
        } else if (command == 11 &&
                   entry.state == PoolEntryState::PENDING_SUCCESSOR) {
            // A confirmed conflict permanently invalidates the successor and
            // releases its target capacity for one bounded replacement.
            entry.state = PoolEntryState::INVALIDATED;
            entry.reservation_id.SetNull();
        } else if (command == 12 && !ledger.records.empty()) {
            const size_t record_index =
                provider.ConsumeIntegralInRange<size_t>(
                    0, ledger.records.size() - 1);
            const ProviderMaintenanceRecord before_record{
                ledger.records[record_index]};
            const int64_t actual_fee =
                provider.ConsumeIntegralInRange<int64_t>(
                    0, before_record.maximum_fee.value + 1);
            const uint256 transaction_id{uint256S(strprintf(
                "%x", ++nonce))};
            std::string error;
            const uint256 before{ObjectHash(ledger)};
            const bool spent = SpendProviderMaintenanceBudget(
                ledger, before_record.operation_id, transaction_id,
                DGBSatoshis{actual_fee}, now, error);
            if (!spent) {
                // Wrong state and over-ceiling actual fees fail atomically.
                assert(ObjectHash(ledger) == before);
            } else {
                const auto& updated = ledger.records[record_index];
                assert(updated.state == ProviderMaintenanceState::BROADCAST ||
                       updated.state == ProviderMaintenanceState::CONFIRMED);
                assert(updated.actual_fee.value <= updated.maximum_fee.value);
                assert(!updated.transaction_id.IsNull());
            }
        } else if (command == 13 && !ledger.records.empty()) {
            ProviderMaintenanceRecord& record = ledger.records[
                provider.ConsumeIntegralInRange<size_t>(
                    0, ledger.records.size() - 1)];
            if (record.state == ProviderMaintenanceState::BROADCAST) {
                // Model chain reconciliation after the maintenance
                // transaction receives its required confirmation.
                record.state = ProviderMaintenanceState::CONFIRMED;
                record.updated_at = std::max(record.updated_at, now);
            }
        } else if (command == 14 && !ledger.records.empty()) {
            ProviderMaintenanceRecord& record = ledger.records[
                provider.ConsumeIntegralInRange<size_t>(
                    0, ledger.records.size() - 1)];
            if (record.state == ProviderMaintenanceState::CONFIRMED) {
                // A reorg restores the unconfirmed liability without
                // changing its transaction or actual-fee binding.
                record.state = ProviderMaintenanceState::BROADCAST;
                record.updated_at = std::max(record.updated_at, now);
            }
        }

        AssertPoolInvariants(pool);
        std::string error;
        assert(ValidateProviderMaintenanceLedger(ledger, error));
        std::set<uint256> operations;
        for (const auto& record : ledger.records) {
            assert(operations.insert(record.operation_id).second);
        }
        for (const PoolPurpose purpose : {PoolPurpose::ADMISSION,
                                          PoolPurpose::OPERATIONAL}) {
            for (const PoolAsset asset : {PoolAsset::DGB,
                                          PoolAsset::DD_CARRIER}) {
                assert(ActiveCount(pool, purpose, asset) <=
                       TargetFor(policy, purpose, asset));
            }
        }
    }
}
