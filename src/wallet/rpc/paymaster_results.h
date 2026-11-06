// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_RPC_PAYMASTER_RESULTS_H
#define DIGIBYTE_WALLET_RPC_PAYMASTER_RESULTS_H

#include <wallet/paymasterresults.h>
#include <wallet/paymasterclientview.h>
#include <univalue.h>

class CConnman;
namespace wallet {
struct ProviderFinanceStatus;
namespace paymaster_app {
struct ProviderActivitySnapshot;
struct ProviderAutomationSnapshot;
} // namespace paymaster_app
} // namespace wallet
namespace wallet::paymaster_rpc::internal {
UniValue ToJSON(const ProviderFinanceStatus& value);
UniValue ToJSON(const paymaster_app::ProviderActivitySnapshot& value);
UniValue ToJSON(const paymaster_app::ProviderAutomationSnapshot& value);
std::string ResultStatusName(DigiDollar::Paymaster::PaymasterResultStatus status);
std::string FeeModeName(DigiDollar::Paymaster::FeeMode mode);
std::string AlternativeRecoveryPhaseName(
    DigiDollar::Paymaster::AlternativeRecoveryPhase phase);
UniValue FundingSafetyLimitsToJSON(
    const DigiDollar::Paymaster::FundingSafetyLimits& limits);
UniValue ProviderSafetyPolicyToJSON(
    const DigiDollar::Paymaster::ProviderSafetyPolicy& policy);
UniValue PaymasterTransportToJSON(const CConnman& connman,
    std::optional<DigiDollar::Paymaster::DirectState> state = std::nullopt);
UniValue ReadinessErrorsToJSON(const std::vector<std::string>& errors);
UniValue ToJSON(const paymaster_app::TransportSnapshot& value);
UniValue ToJSON(const paymaster_app::ProviderStartResult& value);
UniValue ToJSON(const paymaster_app::ProviderStopResult& value);

UniValue ProviderPolicyToJSON(const DigiDollar::Paymaster::ProviderPolicy& policy);
UniValue ClientSafetyPolicyToJSON(const DigiDollar::Paymaster::ClientSafetyPolicy& policy);
UniValue LiquidityPolicyToJSON(const DigiDollar::Paymaster::ProviderLiquidityPolicy& policy);
UniValue ToJSON(const DigiDollar::Paymaster::ProviderIdentityRecord& value);
UniValue ToJSON(const DigiDollar::Paymaster::ProviderPolicy& value);
UniValue ToJSON(const DigiDollar::Paymaster::ProviderSafetyPolicy& value);
UniValue ToJSON(const DigiDollar::Paymaster::ClientSafetyPolicy& value);
UniValue ToJSON(const paymaster_app::ProviderEnabledResult& value);
UniValue ToJSON(const paymaster_app::RestrictedDescriptorResult& value);
UniValue ToJSON(const paymaster_app::ProviderRuntimeResult& value);
UniValue ToJSON(const DigiDollar::Paymaster::ProviderLiquidityPolicy& value);
UniValue ToJSON(const paymaster_app::MaintenanceCancellationResult& value);
UniValue ToJSON(const paymaster_app::CapitalReleaseResult& value);
UniValue ToJSON(const paymaster_app::BackupAdoptionResult& value);
UniValue ToJSON(const paymaster_app::BackupAcknowledgementResult& value);
void AddSessionPaymentStatus(UniValue& result,
                             const DigiDollar::Paymaster::PaymentSession& session,
                             const PaymasterStore* store = nullptr);
UniValue SessionToJSON(
    const DigiDollar::Paymaster::PaymentSession& session,
    const PaymasterStore* store = nullptr);
UniValue AttemptToJSON(const DigiDollar::Paymaster::ProviderAttempt& attempt);
UniValue AlternativeRecoveryToJSON(
    const DigiDollar::Paymaster::AlternativeRecoveryRecord& recovery);
UniValue ToJSON(const paymaster_app::SessionSnapshot& value);
UniValue ToJSON(const paymaster_app::PaymentResult& value);
UniValue ToJSON(const paymaster_app::ProviderProcessingResult& value);
UniValue ClientSessionSnapshotToJSON(PaymasterStore& store, const DigiDollar::Paymaster::PaymentSession& session,
                                     std::string_view action);
UniValue ClientSessionSummaryToJSON(const PaymasterStore& store, const DigiDollar::Paymaster::PaymentSession& session,
                                    const paymaster_app::ClientSessionView& view);
UniValue ToJSON(const paymaster_app::ClientRecoveryResult& value);
void AddPreparationDiagnostic(UniValue& item, const DigiDollar::Paymaster::ProviderMaintenanceRecord& record);
void AddPreparationFunding(UniValue& item, CWallet& wallet, const DigiDollar::Paymaster::ProviderMaintenanceRecord& record);
UniValue PoolPreparationToJSON(CWallet& wallet);
UniValue PoolEntryToJSON(const DigiDollar::Paymaster::ProviderPoolEntry& entry);
UniValue ToJSON(const std::vector<paymaster_app::PreparationSnapshot>& values);
UniValue ToJSON(const paymaster_app::CarrierWithdrawalResult& value);
UniValue ToJSON(const paymaster_app::PoolPreparationResult& value);
UniValue ToJSON(const paymaster_app::PoolRebalanceResult& value);
} // namespace wallet::paymaster_rpc::internal
#endif // DIGIBYTE_WALLET_RPC_PAYMASTER_RESULTS_H
