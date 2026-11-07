// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_PAYMASTERCONFIGURATION_H
#define DIGIBYTE_WALLET_PAYMASTERCONFIGURATION_H
#include <paymaster/provider.h>
#include <wallet/paymasterresults.h>
#include <optional>
namespace wallet {
class CWallet;
struct WalletContext;
namespace paymaster_app {
DigiDollar::Paymaster::ProviderIdentityRecord CreateProviderIdentity(CWallet& wallet_ref, const std::string& display_name);
DigiDollar::Paymaster::ProviderPolicy SaveProviderPolicy(WalletContext& context, CWallet& wallet_ref, DigiDollar::Paymaster::ProviderPolicy policy);
DigiDollar::Paymaster::ProviderSafetyPolicy SaveProviderSafetyPolicy(WalletContext& context, CWallet& wallet_ref, DigiDollar::Paymaster::ProviderSafetyPolicy policy);
DigiDollar::Paymaster::ClientSafetyPolicy SaveClientSafetyPolicy(CWallet& wallet_ref, DigiDollar::Paymaster::ClientSafetyPolicy policy);
ProviderEnabledResult EnableProvider(WalletContext& context, CWallet& wallet_ref, bool requested_enabled);
RestrictedDescriptorResult CreateRestrictedDescriptor(WalletContext& context, CWallet& wallet_ref, const XOnlyPubKey& sponsor_key, const std::string& service_name, int64_t ttl);
ProviderRuntimeResult SaveProviderRuntime(WalletContext& context, CWallet& wallet_ref, std::optional<DigiDollar::Paymaster::ProviderOperationMode> requested_mode, std::optional<bool> requested_autostart);
DigiDollar::Paymaster::ProviderLiquidityPolicy SaveLiquidityPolicy(WalletContext& context, CWallet& wallet_ref, DigiDollar::Paymaster::ProviderLiquidityPolicy policy, std::optional<int64_t> expected_revision, std::optional<uint256> expected_provider_policy_hash = std::nullopt);
MaintenanceCancellationResult CancelProviderMaintenance(WalletContext& context, CWallet& wallet_ref, const std::vector<uint256>& operations, bool execute, const uint256& plan);
CapitalReleaseResult ReleaseProviderCapital(WalletContext& context, CWallet& wallet_ref, bool execute, const uint256& plan, bool recovery, bool cancel_uncreated);
BackupAdoptionResult AdoptProviderBackup(WalletContext& context, CWallet& wallet_ref, bool execute, const uint256& plan, bool independent_checkpoint);
BackupAcknowledgementResult AcknowledgeProviderBackup(CWallet& wallet_ref, bool external_backup);
} // namespace paymaster_app
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERCONFIGURATION_H
