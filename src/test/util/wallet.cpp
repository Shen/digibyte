// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <test/util/wallet.h>

#include <key_io.h>
#include <outputtype.h>
#include <script/standard.h>
#include <util/check.h>
#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#endif

const std::string ADDRESS_dgbrt_UNSPENDABLE = "dgbrt1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqref3c3";

#ifdef ENABLE_WALLET
std::string getnewaddress(wallet::CWallet& w)
{
    constexpr auto output_type = OutputType::BECH32;
    return EncodeDestination(*Assert(w.GetNewDestination(output_type, "")));
}

void importaddress(wallet::CWallet& wallet, const std::string& address)
{
    auto spk_man = wallet.GetLegacyScriptPubKeyMan();
    LOCK2(wallet.cs_wallet, spk_man->cs_KeyStore);
    const auto dest = DecodeDestination(address);
    assert(IsValidDestination(dest));
    const auto script = GetScriptForDestination(dest);
    wallet.MarkDirty();
    assert(!spk_man->HaveWatchOnly(script));
    if (!spk_man->AddWatchOnly(script, 0 /* nCreateTime */)) assert(false);
    wallet.SetAddressBook(dest, /* label */ "", ::wallet::AddressPurpose::RECEIVE);
}
#endif // ENABLE_WALLET
