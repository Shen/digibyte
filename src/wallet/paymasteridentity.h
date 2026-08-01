// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** \file Persistent BIP86 provider identity creation and signing. */

#ifndef DIGIBYTE_WALLET_PAYMASTERIDENTITY_H
#define DIGIBYTE_WALLET_PAYMASTERIDENTITY_H

#include <paymaster/provider.h>

#include <string>

class CKey;

namespace wallet {

class CWallet;

bool CheckPaymasterProviderWallet(const CWallet& wallet, std::string& error);

bool CreatePaymasterIdentity(CWallet& wallet,
                             const std::string& display_name,
                             int64_t now,
                             DigiDollar::Paymaster::ProviderIdentityRecord& identity,
                             std::string& error);

bool GetPaymasterIdentity(const CWallet& wallet,
                          DigiDollar::Paymaster::ProviderIdentityRecord& identity);

bool GetPaymasterIdentityKey(CWallet& wallet,
                             CKey& key,
                             DigiDollar::Paymaster::ProviderIdentityRecord& identity,
                             std::string& error);

} // namespace wallet

#endif // DIGIBYTE_WALLET_PAYMASTERIDENTITY_H
