// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Creation and retrieval of the wallet-scoped persistent provider identity.
 * Provider operation requires a descriptor wallet with local private keys so
 * identity and BIP86 control proofs never depend on an external signer.
 */

#include <wallet/paymasteridentity.h>

#include <chainparams.h>
#include <key.h>
#include <paymaster/directory.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>

namespace wallet {
using DigiDollar::Paymaster::ProviderBackupStatus;
using DigiDollar::Paymaster::ProviderIdentityRecord;

bool CheckPaymasterProviderWallet(const CWallet& wallet, std::string& error)
{
    error.clear();
    if (!wallet.IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
        error = "PAYMASTER_PROVIDER_REQUIRES_DESCRIPTOR_WALLET";
        return false;
    }
    if (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        error = "PAYMASTER_PROVIDER_REQUIRES_PRIVATE_KEYS";
        return false;
    }
    if (wallet.IsWalletFlagSet(WALLET_FLAG_EXTERNAL_SIGNER)) {
        error = "PAYMASTER_PROVIDER_REJECTS_EXTERNAL_SIGNER";
        return false;
    }
    return true;
}

bool GetPaymasterIdentity(const CWallet& wallet, ProviderIdentityRecord& identity)
{
    LOCK(wallet.cs_wallet);
    return WalletBatch{wallet.GetDatabase()}.ReadPaymasterIdentity(identity);
}

bool GetPaymasterIdentityKey(CWallet& wallet,
                             CKey& key,
                             ProviderIdentityRecord& identity,
                             std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    LOCK(wallet.cs_wallet);
    if (!CheckPaymasterProviderWallet(wallet, error)) return false;
    if (wallet.IsLocked()) {
        error = "PAYMASTER_WALLET_LOCKED";
        return false;
    }
    if (!WalletBatch{wallet.GetDatabase()}.ReadPaymasterIdentity(identity)) {
        error = "PAYMASTER_IDENTITY_NOT_FOUND";
        return false;
    }
    for (ScriptPubKeyMan* spk_man : wallet.GetScriptPubKeyMans(identity.identity_script)) {
        auto* descriptor = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man);
        if (!descriptor) continue;
        auto provider = descriptor->GetSigningProviderWithKeys(identity.identity_script);
        if (provider && provider->GetKeyByXOnly(identity.identity_key, key) && key.IsValid() &&
            XOnlyPubKey{key.GetPubKey()} == identity.identity_key) return true;
    }
    error = "PAYMASTER_IDENTITY_KEY_UNAVAILABLE";
    return false;
}

bool CreatePaymasterIdentity(CWallet& wallet,
                             const std::string& display_name,
                             int64_t now,
                             ProviderIdentityRecord& identity,
                             std::string& error)
{
    using namespace DigiDollar::Paymaster;
    error.clear();
    LOCK(wallet.cs_wallet);
    if (!CheckPaymasterProviderWallet(wallet, error)) return false;
    if (!IsValidPaymasterDisplayName(display_name) || now <= 0) {
        error = "PAYMASTER_INVALID_IDENTITY_METADATA";
        return false;
    }
    if (wallet.IsLocked()) {
        error = "PAYMASTER_WALLET_LOCKED";
        return false;
    }
    WalletBatch batch{wallet.GetDatabase()};
    if (batch.ReadPaymasterIdentity(identity)) return true;

    const auto destination = wallet.GetNewDestination(OutputType::BECH32M, "Paymaster identity");
    if (!destination) {
        error = "PAYMASTER_IDENTITY_DESTINATION_UNAVAILABLE";
        return false;
    }
    const auto* taproot = std::get_if<WitnessV1Taproot>(&*destination);
    if (!taproot) {
        error = "PAYMASTER_IDENTITY_NOT_BIP86";
        return false;
    }
    const CScript script = GetScriptForDestination(*destination);
    const XOnlyPubKey output_key{*taproot};
    for (ScriptPubKeyMan* spk_man : wallet.GetScriptPubKeyMans(script)) {
        auto* descriptor = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man);
        if (!descriptor) continue;
        auto provider = descriptor->GetSigningProviderWithKeys(script);
        TaprootSpendData spend_data;
        CKey key;
        if (!provider || !provider->GetTaprootSpendData(output_key, spend_data) ||
            !spend_data.internal_key.IsFullyValid() || !spend_data.merkle_root.IsNull() ||
            !spend_data.scripts.empty() ||
            !provider->GetKeyByXOnly(spend_data.internal_key, key) || !key.IsValid()) continue;

        identity.provider_id = GetPaymasterId(spend_data.internal_key);
        identity.identity_key = spend_data.internal_key;
        identity.identity_script = script;
        identity.display_name = display_name;
        identity.created_at = now;
        ProviderBackupStatus backup_status;
        backup_status.genesis_hash = Params().GenesisBlock().GetHash();
        backup_status.provider_id = identity.provider_id;
        backup_status.identity_created_at = now;
        backup_status.reminder_updated_at = now;
        ProviderFinanceLedger finance_ledger;
        finance_ledger.genesis_hash = Params().GenesisBlock().GetHash();
        finance_ledger.provider_id = identity.provider_id;
        finance_ledger.history_complete_from = now;
        finance_ledger.updated_at = now;
        std::string ledger_error;
        if (!RebuildProviderFinanceDailyTotals(finance_ledger, ledger_error)) {
            identity = {};
            error = ledger_error;
            return false;
        }
        if (!batch.TxnBegin()) {
            identity = {};
            error = "PAYMASTER_IDENTITY_DATABASE_BEGIN";
            return false;
        }
        if (!batch.WritePaymasterIdentity(identity, false) ||
            !batch.WritePaymasterBackupStatus(backup_status, false) ||
            !batch.WritePaymasterFinanceLedger(finance_ledger, false)) {
            batch.TxnAbort();
            identity = {};
            error = "PAYMASTER_IDENTITY_DATABASE_WRITE";
            return false;
        }
        if (!batch.TxnCommit()) {
            identity = {};
            error = "PAYMASTER_IDENTITY_DATABASE_COMMIT";
            return false;
        }
        return true;
    }
    error = "PAYMASTER_IDENTITY_KEY_UNAVAILABLE";
    return false;
}

bool GetPaymasterProviderBackupStatus(const CWallet& wallet,
                                      ProviderBackupStatus& status,
                                      int64_t now,
                                      std::string& error)
{
    error.clear();
    LOCK(wallet.cs_wallet);
    WalletBatch batch{wallet.GetDatabase()};
    ProviderIdentityRecord identity;
    if (!batch.ReadPaymasterIdentity(identity)) {
        error = "PAYMASTER_IDENTITY_NOT_FOUND";
        return false;
    }
    if (batch.ReadPaymasterBackupStatus(status)) {
        if (status.genesis_hash != Params().GenesisBlock().GetHash() ||
            status.provider_id != identity.provider_id) {
            error = "PAYMASTER_BACKUP_STATUS_BINDING_MISMATCH";
            return false;
        }
        return true;
    }
    if (batch.HasPaymasterBackupStatus()) {
        error = "PAYMASTER_INVALID_BACKUP_STATUS";
        return false;
    }
    // Wallets created before the reminder feature have no trustworthy proof
    // of a post-identity backup. Start with a visible, non-blocking reminder.
    status.genesis_hash = Params().GenesisBlock().GetHash();
    status.provider_id = identity.provider_id;
    status.identity_created_at = identity.created_at;
    status.reminder_updated_at = std::max(identity.created_at, now);
    if (!batch.WritePaymasterBackupStatus(status, false)) {
        error = "PAYMASTER_BACKUP_STATUS_DATABASE_WRITE";
        return false;
    }
    return true;
}

bool MarkPaymasterProviderBackupCompleted(const CWallet& wallet,
                                          int64_t now,
                                          std::string& error)
{
    ProviderBackupStatus status;
    if (!GetPaymasterProviderBackupStatus(wallet, status, now, error)) {
        // A wallet without a provider identity has no Paymaster reminder to
        // update; its ordinary backup remains successful.
        return error == "PAYMASTER_IDENTITY_NOT_FOUND";
    }
    LOCK(wallet.cs_wallet);
    status.last_successful_backup_at = std::max(
        status.last_successful_backup_at, now);
    if (!WalletBatch{wallet.GetDatabase()}.WritePaymasterBackupStatus(status)) {
        error = "PAYMASTER_BACKUP_STATUS_DATABASE_WRITE";
        return false;
    }
    error.clear();
    return true;
}

bool AcknowledgePaymasterProviderExternalBackup(CWallet& wallet,
                                                int64_t now,
                                                std::string& error)
{
    ProviderBackupStatus status;
    if (!GetPaymasterProviderBackupStatus(wallet, status, now, error)) return false;
    LOCK(wallet.cs_wallet);
    status.external_backup_acknowledged_at = std::max(
        status.external_backup_acknowledged_at, now);
    if (!WalletBatch{wallet.GetDatabase()}.WritePaymasterBackupStatus(status)) {
        error = "PAYMASTER_BACKUP_STATUS_DATABASE_WRITE";
        return false;
    }
    error.clear();
    return true;
}

} // namespace wallet
