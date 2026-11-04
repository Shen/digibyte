// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef DIGIBYTE_WALLET_PAYMASTERPOOL_H
#define DIGIBYTE_WALLET_PAYMASTERPOOL_H
#include <paymaster/provider.h>
#include <univalue.h>
namespace wallet {
class CWallet;
struct WalletContext;
void RunPaymasterPoolPreparation(WalletContext& context, CWallet& wallet);
namespace paymaster_app {
uint256 PoolPreparationAuthorization(CWallet& wallet);
DigiDollar::Paymaster::ProviderMaintenanceLedger ReadPreparationLedger(CWallet& wallet);
void WritePreparationLedger(CWallet& wallet, const DigiDollar::Paymaster::ProviderMaintenanceLedger& ledger);
void AddPreparationDiagnostic(UniValue& item, const DigiDollar::Paymaster::ProviderMaintenanceRecord& record);
void AddPreparationFunding(UniValue& item, CWallet& wallet, const DigiDollar::Paymaster::ProviderMaintenanceRecord& record);
void ContinuePoolPreparation(CWallet& wallet);
bool HasPendingPoolPreparation(CWallet& wallet);
UniValue PoolPreparationToJSON(CWallet& wallet);
} // namespace paymaster_app
} // namespace wallet
#endif // DIGIBYTE_WALLET_PAYMASTERPOOL_H
