// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_PAYMASTERPROVIDEROPS_H
#define DIGIBYTE_WALLET_PAYMASTERPROVIDEROPS_H
#include <paymaster/provider.h>
#include <primitives/transaction.h>
#include <univalue.h>
#include <optional>

namespace wallet {
class CWallet;
struct WalletContext;
namespace paymaster_app {
UniValue PoolEntryToJSON(const DigiDollar::Paymaster::ProviderPoolEntry& entry);
uint256 CarrierWithdrawalPlanId(const DigiDollar::Paymaster::ProviderCarrierWithdrawalPlan& plan);
struct PoolTargets {
    int admission{0};
    int operational{0};
    int admission_carriers{0};
    int operational_carriers{0};
    bool execute{false};
    std::optional<uint256> plan_id;
};
struct PoolRecoveryOutput {
    uint32_t index{0};
    DigiDollar::Paymaster::PoolPurpose purpose{DigiDollar::Paymaster::PoolPurpose::ADMISSION};
};
struct PoolPreparationParameters : PoolTargets {
    bool cancel{false};
    CAmount maximum_fee{20000000};
    std::optional<DigiDollar::Paymaster::ProviderPolicy> preview_policy;
    std::optional<uint256> recovery_transaction;
    std::optional<std::vector<PoolRecoveryOutput>> recovery_outputs;
};
struct PoolRebalanceParameters : PoolTargets {
    bool dgb_only{false};
    bool recommend_fee{false};
    std::optional<CAmount> maximum_fee;
    std::optional<int64_t> expected_revision;
};
struct CarrierWithdrawalParameters {
    DigiDollar::Paymaster::CarrierWithdrawalMode mode{DigiDollar::Paymaster::CarrierWithdrawalMode::ALL_EXCESS};
    bool execute{false};
    std::optional<uint256> plan_id;
    std::optional<COutPoint> source;
};
UniValue PreparePaymasterPool(WalletContext& context, CWallet& wallet, const PoolPreparationParameters& parameters);
UniValue RebalancePaymasterPool(WalletContext& context, CWallet& wallet, const PoolRebalanceParameters& parameters);
UniValue WithdrawPaymasterCarrier(WalletContext& context, CWallet& wallet, const CarrierWithdrawalParameters& parameters);
} // namespace paymaster_app
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERPROVIDEROPS_H
