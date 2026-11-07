// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_WALLET_PAYMASTERAPPLICATION_H
#define DIGIBYTE_WALLET_PAYMASTERAPPLICATION_H

#include <paymaster/manager.h>
#include <wallet/paymasterprovider.h>
#include <wallet/paymasterfinance.h>

#include <optional>
#include <string>
#include <vector>

namespace wallet {
class CWallet;
class WalletBatch;
class PaymasterStore;
bool EnsureProviderNotEquivocationBlocked(PaymasterStore& store,
    const DigiDollar::Paymaster::PaymasterId& provider_id, std::string& error);
struct WalletContext;
namespace paymaster_app {
class ProviderWorkGuard final
{
public:
    ProviderWorkGuard(DigiDollar::Paymaster::Manager& manager,
                      const std::string& wallet_name,
                      const DigiDollar::Paymaster::PaymasterId& provider_id,
                      bool require_running = true)
        : m_manager{manager}, m_wallet_name{wallet_name},
          m_provider_id{provider_id},
          m_acquired{manager.TryBeginProviderWork(
              wallet_name, provider_id, require_running)}
    {
    }

    ProviderWorkGuard(const ProviderWorkGuard&) = delete;
    ProviderWorkGuard& operator=(const ProviderWorkGuard&) = delete;
    ~ProviderWorkGuard()
    {
        if (m_acquired) m_manager.EndProviderWork(m_wallet_name);
    }
    bool Acquired() const noexcept { return m_acquired; }
    bool CompleteProviderStart()
    {
        return m_acquired &&
            m_manager.CompleteProviderStart(m_wallet_name, m_provider_id);
    }

private:
    DigiDollar::Paymaster::Manager& m_manager;
    const std::string m_wallet_name;
    const DigiDollar::Paymaster::PaymasterId m_provider_id;
    const bool m_acquired;
};

struct ProviderReadiness {
    bool have_settings{false};
    bool have_identity{false};
    bool have_policy{false};
    bool have_safety_policy{false};
    bool have_budget_ledger{false};
    bool have_liquidity_policy{false};
    bool have_maintenance_ledger{false};
    bool have_pool{false};
    bool wallet_eligible{false};
    bool pool_ready{false};
    bool ready{false};
    uint8_t available_funding_models{0};
    DigiDollar::Paymaster::ProviderSettings settings;
    DigiDollar::Paymaster::ProviderIdentityRecord identity;
    DigiDollar::Paymaster::ProviderPolicy policy;
    DigiDollar::Paymaster::ProviderSafetyPolicy safety_policy;
    DigiDollar::Paymaster::ProviderBudgetLedger budget_ledger;
    DigiDollar::Paymaster::ProviderLiquidityPolicy liquidity_policy;
    DigiDollar::Paymaster::ProviderMaintenanceLedger maintenance_ledger;
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry> pool_entries;
    DigiDollar::Paymaster::ProviderPoolReadiness pool;
    CService endpoint;
    std::vector<std::string> errors;
};

struct ProviderActivitySnapshot {
    uint64_t active_payments{0};
    uint64_t capacity_requests{0};
    uint64_t reserved_outputs{0};
    uint64_t pending_confirmations{0};
};

enum class ProviderAutomationState { READY, OFF, BLOCKED, PAUSED, WAITING_CONFIRMATION, WORKING };

struct ProviderAutomationSnapshot {
    bool configured{false};
    bool enabled{false};
    bool paid_maintenance_approved{false};
    ProviderAutomationState state{ProviderAutomationState::READY};
    std::string reason;
    bool pending_transaction{false};
};

ProviderActivitySnapshot CaptureProviderActivity(const ProviderReadiness& readiness, int64_t now);
/** Observe saved approval and current operation without changing either. */
ProviderAutomationSnapshot CaptureProviderAutomationStatus(
    const CWallet& wallet, const WalletContext& context,
    const ProviderReadiness& readiness, int64_t now, bool synchronized = false);

struct LiquiditySlotCounts {
    size_t target_counted{0};
    size_t ready{0};
    size_t pending{0};
    size_t missing{0};
};

enum class EquivocationBlockPolicy : uint8_t {
    OBSERVE_ONLY,
    ENFORCE,
};

bool ReadOptionalProviderPool(
    WalletBatch& batch,
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    std::string& error);
const char* ProviderOperationModeName(
    DigiDollar::Paymaster::ProviderOperationMode mode);
const char* ProviderServiceStateName(
    DigiDollar::Paymaster::ProviderServiceState state);
std::string StableProviderServiceError(std::string error);
bool AutomaticProviderStateIsSynchronized(CWallet& wallet);
bool ProviderTxIndexIsReady(CWallet& wallet, bool wait_for_sync);
bool CapacityContinuationMayProceed(const ProviderReadiness& readiness);
bool CapacityRequestMayProceed(const ProviderReadiness& readiness,
                               bool have_stored_response);
bool CheckPaymasterClientReadiness(CWallet& wallet,
                                   WalletContext& context,
                                   std::string& error);
DigiDollar::Paymaster::DDCents EffectiveClientServiceFeeCap(
    CWallet& wallet,
    DigiDollar::Paymaster::DDCents requested,
    std::string& error);
bool CheckPaymasterPrivacyReadiness(
    DigiDollar::Paymaster::PrivacyProfile privacy,
    WalletContext& context,
    std::string& error);
/** A persisted refresh reloads the pool under cs_wallet. Supplied entries are
 * only an observation, never authority to overwrite newer reservations. */
bool RefreshPoolConfirmationHeights(
    CWallet& wallet,
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    std::string& error, bool persist = true);
ProviderReadiness GetProviderReadiness(CWallet& wallet,
                                       WalletContext& context,
                                       bool wait_for_sync = true, bool reconcile = true);
bool PublishCurrentProviderAnnouncement(
    CWallet& wallet,
    WalletContext& context,
    const ProviderReadiness& readiness,
    int64_t now,
    DigiDollar::Paymaster::Announcement* published,
    std::string& error);
LiquiditySlotCounts CountLiquiditySlots(
    const std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    DigiDollar::Paymaster::PoolPurpose purpose,
    DigiDollar::Paymaster::PoolAsset asset,
    size_t target,
    int64_t minimum_dgb_value = 0);
DigiDollar::Paymaster::ProviderLiquidityPolicy SuggestedLiquidityPolicy(
    const ProviderReadiness& readiness,
    int64_t now);
DigiDollar::Paymaster::ProviderServiceStatus ProviderServiceStatusForObservation(
    DigiDollar::Paymaster::ProviderServiceStatus status, bool ready, bool synchronized);
bool MakeProviderPoolRoomForMaintenance(
    std::vector<DigiDollar::Paymaster::ProviderPoolEntry>& entries,
    size_t additional,
    std::string& error);
bool SaveCarrierWithdrawalPreview(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan,
    std::string& error);
bool ReconcileProviderMaintenance(CWallet& wallet,
                                  size_t& recovered,
                                  std::string& error);
bool ReleaseSatisfiedProviderReplenishment(CWallet& wallet, bool dgb_satisfied,
                                          bool carriers_satisfied, std::string& error);
bool RunAutomaticDGBReplenishment(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    size_t missing_admission,
    size_t missing_operational,
    std::string& error);
bool RunAutomaticCarrierReplenishment(
    CWallet& wallet,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy,
    size_t missing_admission,
    size_t missing_operational,
    std::string& error);
uint256 MaintenancePlanId(
    DigiDollar::Paymaster::ProviderMaintenanceKind kind,
    const std::vector<DigiDollar::Paymaster::ProviderMaintenanceOutput>& outputs,
    const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy);
std::optional<uint32_t> FindMaintenanceOutputIndex(
    const CTransaction& transaction,
    const DigiDollar::Paymaster::ProviderMaintenanceOutput& output);
std::optional<uint32_t> FindExactDigiDollarOutputIndex(
    const CTransaction& transaction,
    const CScript& script_pub_key,
    DigiDollar::Paymaster::DDCents amount);
void EnsureProviderRestoreSafe(const CWallet& wallet);
bool ProviderCanServeDuringRefill(const DigiDollar::Paymaster::ProviderServiceStatus& status, const ProviderReadiness& readiness, bool synchronized);
} // namespace paymaster_app
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERAPPLICATION_H
