// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file
 * Creation and retrieval of the wallet-scoped persistent provider identity.
 * Provider operation requires a descriptor wallet with local private keys so
 * identity and BIP86 control proofs never depend on an external signer.
 */

#include <wallet/paymasteridentity.h>

#include <key.h>
#include <paymaster/directory.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

namespace wallet {
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
        if (!batch.WritePaymasterIdentity(identity, false)) {
            identity = {};
            error = "PAYMASTER_IDENTITY_DATABASE_WRITE";
            return false;
        }
        return true;
    }
    error = "PAYMASTER_IDENTITY_KEY_UNAVAILABLE";
    return false;
}

} // namespace wallet
