// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <protocol.h>

#include <chainparams.h>
#include <common/system.h>
#include <oracle/musig2_messages.h>
#include <primitives/oracle.h>
#include <hash.h>

#include <atomic>

static std::atomic<bool> g_initial_block_download_completed(false);

namespace NetMsgType {
const char* VERSION = "version";
const char* VERACK = "verack";
const char* ADDR = "addr";
const char* ADDRV2 = "addrv2";
const char* SENDADDRV2 = "sendaddrv2";
const char* INV = "inv";
const char* GETDATA = "getdata";
const char* MERKLEBLOCK = "merkleblock";
const char* GETBLOCKS = "getblocks";
const char* GETHEADERS = "getheaders";
const char* TX = "tx";
const char* HEADERS = "headers";
const char* BLOCK = "block";
const char* GETADDR = "getaddr";
const char* MEMPOOL = "mempool";
const char* PING = "ping";
const char* PONG = "pong";
const char* NOTFOUND = "notfound";
const char* FILTERLOAD = "filterload";
const char* FILTERADD = "filteradd";
const char* FILTERCLEAR = "filterclear";
const char* SENDHEADERS = "sendheaders";
const char* FEEFILTER = "feefilter";
const char* SENDCMPCT = "sendcmpct";
const char* CMPCTBLOCK = "cmpctblock";
const char* GETBLOCKTXN = "getblocktxn";
const char* BLOCKTXN = "blocktxn";
const char* DANDELIONTX = "dandeliontx";
const char* GETCFILTERS = "getcfilters";
const char* CFILTER = "cfilter";
const char* GETCFHEADERS = "getcfheaders";
const char* CFHEADERS = "cfheaders";
const char* GETCFCHECKPT = "getcfcheckpt";
const char* CFCHECKPT = "cfcheckpt";
const char* WTXIDRELAY = "wtxidrelay";
const char* SENDTXRCNCL = "sendtxrcncl";
const char* ORACLEPRICE = "oracleprice";
const char* ORACLEBUNDLE = "oraclebundle";
const char* GETORACLES = "getoracles";
const char* ORACLECONSENSUS = "oracleconsns";
const char* ORACLEATTESTATION = "oracleattest";
const char* ORACLEMUSIGNONCE = "oramusnonce";
const char* ORACLEMUSIGCONTEXT = "oramusigctx";
const char* ORACLEMUSIGPARTIALSIG = "oramusigpsig";
const char* ORACLEHEARTBEAT = "oraclehb";
const char* SENDPMASTERS = "sendpmasters";
const char* PMANNOUNCE = "pmannounce";
const char* GETPMASTERS = "getpmasters";
const char* PMCAPREQ = "pmcapreq";
const char* PMCAPRESP = "pmcapresp";
const char* PMQUOTEREQ = "pmquotereq";
const char* PMQUOTERESP = "pmquoteresp";
const char* PMSUBMIT = "pmsubmit";
const char* PMRESULT = "pmresult";
const char* PMRECOVERYREQ = "pmrecreq";
const char* PMRECOVERYRESP = "pmrecresp";
const char* PMRECOVERYSUBMIT = "pmrecsub";
const char* PMRECOVERYRESULT = "pmrecresult";

bool IsPaymasterDirectMessage(const std::string& message_type)
{
    return message_type == PMCAPREQ || message_type == PMCAPRESP ||
           message_type == PMQUOTEREQ || message_type == PMQUOTERESP ||
           message_type == PMSUBMIT || message_type == PMRESULT ||
           message_type == PMRECOVERYREQ || message_type == PMRECOVERYRESP ||
           message_type == PMRECOVERYSUBMIT || message_type == PMRECOVERYRESULT;
}

bool IsPaymasterConnectionMessage(const std::string& message_type)
{
    return message_type == VERSION || message_type == VERACK ||
           message_type == SENDPMASTERS || message_type == PING ||
           message_type == PONG || IsPaymasterDirectMessage(message_type);
}
} // namespace NetMsgType

/** All known message types. Keep this in the same order as the list of
 * messages above and in protocol.h.
 */
const static std::vector<std::string> g_all_net_message_types{
    NetMsgType::VERSION,
    NetMsgType::VERACK,
    NetMsgType::ADDR,
    NetMsgType::ADDRV2,
    NetMsgType::SENDADDRV2,
    NetMsgType::INV,
    NetMsgType::GETDATA,
    NetMsgType::MERKLEBLOCK,
    NetMsgType::GETBLOCKS,
    NetMsgType::GETHEADERS,
    NetMsgType::TX,
    NetMsgType::HEADERS,
    NetMsgType::BLOCK,
    NetMsgType::GETADDR,
    NetMsgType::MEMPOOL,
    NetMsgType::PING,
    NetMsgType::PONG,
    NetMsgType::NOTFOUND,
    NetMsgType::FILTERLOAD,
    NetMsgType::FILTERADD,
    NetMsgType::FILTERCLEAR,
    NetMsgType::SENDHEADERS,
    NetMsgType::FEEFILTER,
    NetMsgType::SENDCMPCT,
    NetMsgType::CMPCTBLOCK,
    NetMsgType::GETBLOCKTXN,
    NetMsgType::BLOCKTXN,
    NetMsgType::DANDELIONTX,
    NetMsgType::GETCFILTERS,
    NetMsgType::CFILTER,
    NetMsgType::GETCFHEADERS,
    NetMsgType::CFHEADERS,
    NetMsgType::GETCFCHECKPT,
    NetMsgType::CFCHECKPT,
    NetMsgType::WTXIDRELAY,
    NetMsgType::SENDTXRCNCL,
    NetMsgType::ORACLEPRICE,
    NetMsgType::ORACLEBUNDLE,
    NetMsgType::GETORACLES,
    NetMsgType::ORACLECONSENSUS,
    NetMsgType::ORACLEATTESTATION,
    NetMsgType::ORACLEMUSIGNONCE,
    NetMsgType::ORACLEMUSIGCONTEXT,
    NetMsgType::ORACLEMUSIGPARTIALSIG,
    NetMsgType::ORACLEHEARTBEAT,
    NetMsgType::SENDPMASTERS,
    NetMsgType::PMANNOUNCE,
    NetMsgType::GETPMASTERS,
    NetMsgType::PMCAPREQ,
    NetMsgType::PMCAPRESP,
    NetMsgType::PMQUOTEREQ,
    NetMsgType::PMQUOTERESP,
    NetMsgType::PMSUBMIT,
    NetMsgType::PMRESULT,
    NetMsgType::PMRECOVERYREQ,
    NetMsgType::PMRECOVERYRESP,
    NetMsgType::PMRECOVERYSUBMIT,
    NetMsgType::PMRECOVERYRESULT,
};

CMessageHeader::CMessageHeader(const MessageStartChars& pchMessageStartIn, const char* pszCommand, unsigned int nMessageSizeIn)
{
    pchMessageStart = pchMessageStartIn;

    // Copy the command name
    size_t i = 0;
    for (; i < COMMAND_SIZE && pszCommand[i] != 0; ++i) pchCommand[i] = pszCommand[i];
    assert(pszCommand[i] == 0); // Assert that the command name passed in is not longer than COMMAND_SIZE

    nMessageSize = nMessageSizeIn;
}

std::string CMessageHeader::GetCommand() const
{
    return std::string(pchCommand, pchCommand + strnlen(pchCommand, COMMAND_SIZE));
}

bool CMessageHeader::IsCommandValid() const
{
    // Check the command string for errors
    for (const char* p1 = pchCommand; p1 < pchCommand + COMMAND_SIZE; ++p1) {
        if (*p1 == 0) {
            // Must be all zeros after the first zero
            for (; p1 < pchCommand + COMMAND_SIZE; ++p1) {
                if (*p1 != 0) {
                    return false;
                }
            }
        } else if (*p1 < ' ' || *p1 > 0x7E) {
            return false;
        }
    }

    return true;
}


ServiceFlags GetDesirableServiceFlags(ServiceFlags services) {
    if ((services & NODE_NETWORK_LIMITED) && g_initial_block_download_completed) {
        return ServiceFlags(NODE_NETWORK_LIMITED | NODE_WITNESS);
    }
    return ServiceFlags(NODE_NETWORK | NODE_WITNESS);
}

void SetServiceFlagsIBDCache(bool state) {
    g_initial_block_download_completed = state;
}

CInv::CInv()
{
    type = 0;
    hash.SetNull();
}

CInv::CInv(uint32_t typeIn, const uint256& hashIn) : type(typeIn), hash(hashIn) {}

bool operator<(const CInv& a, const CInv& b)
{
    return (a.type < b.type || (a.type == b.type && a.hash < b.hash));
}

std::string CInv::GetCommand() const
{
    std::string cmd;

    // Check for oracle messages first (they're outside MSG_TYPE_MASK range)
    switch (type)
    {
    case MSG_ORACLE_PRICE:       return NetMsgType::ORACLEPRICE;
    case MSG_ORACLE_BUNDLE:      return NetMsgType::ORACLEBUNDLE;
    case MSG_GET_ORACLE_DATA:    return NetMsgType::GETORACLES;
    case MSG_ORACLE_CONSENSUS:   return NetMsgType::ORACLECONSENSUS;
    case MSG_ORACLE_ATTESTATION: return NetMsgType::ORACLEATTESTATION;
    case MSG_ORACLE_MUSIG_NONCE: return NetMsgType::ORACLEMUSIGNONCE;
    case MSG_ORACLE_MUSIG_CONTEXT: return NetMsgType::ORACLEMUSIGCONTEXT;
    case MSG_ORACLE_MUSIG_PARTIALSIG:
        return NetMsgType::ORACLEMUSIGPARTIALSIG;
    case MSG_ORACLE_HEARTBEAT:
        return NetMsgType::ORACLEHEARTBEAT;
    }

    // Handle witness flag for standard messages
    if (type & MSG_WITNESS_FLAG)
        cmd.append("witness-");
    int masked = type & MSG_TYPE_MASK;
    switch (masked)
    {
    case MSG_TX:             return cmd.append(NetMsgType::TX);
    // WTX is not a message type, just an inv type
    case MSG_WTX:            return cmd.append("wtx");
    case MSG_BLOCK:          return cmd.append(NetMsgType::BLOCK);
    case MSG_FILTERED_BLOCK: return cmd.append(NetMsgType::MERKLEBLOCK);
    case MSG_CMPCT_BLOCK:    return cmd.append(NetMsgType::CMPCTBLOCK);
    case MSG_DANDELION_TX:   return cmd.append(NetMsgType::DANDELIONTX);
    default:
        throw std::out_of_range(strprintf("CInv::GetCommand(): type=%d unknown type", type));
    }
}

std::string CInv::ToString() const
{
    try {
        return strprintf("%s %s", GetCommand(), hash.ToString());
    } catch(const std::out_of_range &) {
        return strprintf("0x%08x %s", type, hash.ToString());
    }
}

const std::vector<std::string> &getAllNetMessageTypes()
{
    return g_all_net_message_types;
}

/**
 * Convert a service flag (NODE_*) to a human readable string.
 * It supports unknown service flags which will be returned as "UNKNOWN[...]".
 * @param[in] bit the service flag is calculated as (1 << bit)
 */
static std::string serviceFlagToStr(size_t bit)
{
    const uint64_t service_flag = 1ULL << bit;
    switch ((ServiceFlags)service_flag) {
    case NODE_NONE: abort();  // impossible
    case NODE_NETWORK:         return "NETWORK";
    case NODE_BLOOM:           return "BLOOM";
    case NODE_WITNESS:         return "WITNESS";
    case NODE_COMPACT_FILTERS: return "COMPACT_FILTERS";
    case NODE_NETWORK_LIMITED: return "NETWORK_LIMITED";
    case NODE_P2P_V2:          return "P2P_V2";
    // Not using default, so we get warned when a case is missing
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "UNKNOWN[";
    stream << "2^" << bit;
    stream << "]";
    return stream.str();
}

std::vector<std::string> serviceFlagsToStr(uint64_t flags)
{
    std::vector<std::string> str_flags;

    for (size_t i = 0; i < sizeof(flags) * 8; ++i) {
        if (flags & (1ULL << i)) {
            str_flags.emplace_back(serviceFlagToStr(i));
        }
    }

    return str_flags;
}

GenTxid ToGenTxid(const CInv& inv)
{
    assert(inv.IsGenTxMsg());
    return inv.IsMsgWtx() ? GenTxid::Wtxid(inv.hash) : GenTxid::Txid(inv.hash);
}

uint256 OraclePriceMsg::GetHash() const
{
    // Use the compact attestation hash (oracle_id + price + timestamp) for
    // dedup. GetSignatureHash() includes block_height+nonce, which are not
    // covered by attestations. An attacker can mutate those fields to create
    // distinct hashes that bypass dedup while the attestation remains valid.
    // The compact hash maps those mutations to one dedup key.
    if (!price_message.schnorr_sig.empty()) {
        return price_message.GetAttestationSignatureHash();
    }
    // Fallback for unsigned storage-only messages.
    return price_message.GetSignatureHash();
}

uint256 OracleBundleMsg::GetHash() const
{
    CHashWriter hasher(0);
    hasher << std::string{"oracle-bundle-v1"};
    hasher << bundle;
    return hasher.GetHash();
}

uint256 OracleMusigNonceMsg::GetHash() const
{
    CHashWriter hasher(0);
    hasher << epoch;
    hasher << attempt_id;
    hasher << oracle_id;
    hasher << pubnonce;
    return hasher.GetHash();
}

bool IsAuthorizedMuSig2OracleIdForRelay(const CChainParams& params, uint32_t oracle_id)
{
    const Consensus::Params& consensus = params.GetConsensus();
    if (consensus.nOraclePubkeyCount <= 0) return false;
    if (oracle_id >= static_cast<uint32_t>(consensus.nOraclePubkeyCount)) return false;
    if (consensus.vOraclePublicKeys.size() < static_cast<size_t>(consensus.nOraclePubkeyCount)) return false;

    const OracleNodeInfo* oracle_config = params.GetOracleNode(oracle_id);
    return oracle_config && oracle_config->is_active;
}

bool IsMuSig2RelayEpochInRange(int32_t message_epoch, int32_t current_epoch)
{
    return message_epoch >= 0 &&
           message_epoch >= current_epoch &&
           message_epoch <= current_epoch + 1;
}

uint256 OracleMusigNonceMsg::GetSignatureHash() const
{
    // Tagged hash for authentication:
    // "DigiDollar/MuSig2Nonce" || hashGenesisBlock || epoch || attempt_id || oracle_id || pubnonce.
    // Binding the active chain identity prevents captured nonce auth from one
    // network being replayed into another network that shares the oracle roster.
    CHashWriter hasher(0);
    hasher << std::string("DigiDollar/MuSig2Nonce");
    hasher << Params().GetConsensus().hashGenesisBlock;
    hasher << epoch;
    hasher << attempt_id;
    hasher << oracle_id;
    hasher << pubnonce;
    return hasher.GetHash();
}

bool OracleMusigNonceMsg::Sign(const CKey& key)
{
    uint256 hash = GetSignatureHash();
    signature.resize(64);
    if (!key.SignSchnorr(hash, signature, nullptr, uint256())) {
        signature.clear();
        return false;
    }
    return true;
}

bool OracleMusigNonceMsg::VerifySignature(const XOnlyPubKey& pubkey) const
{
    if (signature.size() != 64) return false;
    if (!pubkey.IsFullyValid()) return false;
    uint256 hash = GetSignatureHash();
    return pubkey.VerifySchnorr(hash, signature);
}

uint256 OracleMusigPartialSigMsg::GetHash() const
{
    CHashWriter hasher(0);
    hasher << std::string("DigiDollar/MuSig2PartialSigMsg/v2");
    hasher << Params().GetConsensus().hashGenesisBlock;
    hasher << epoch;
    hasher << attempt_id;
    hasher << context_version;
    hasher << session_context_id;
    hasher << oracle_id;
    hasher << partial_sig;
    return hasher.GetHash();
}

uint256 OracleMusigPartialSigMsg::GetSignatureHash() const
{
    // Tagged hash for authentication:
    // "DigiDollar/MuSig2PartialSig" || hashGenesisBlock || epoch ||
    // attempt_id || context_version || session_context_id || oracle_id || partial_sig.
    // session_context_id binds the relay auth to the exact MuSig2 transcript:
    // bundle message hash, frozen signer bitmap, and nonce set.
    CHashWriter hasher(0);
    hasher << std::string("DigiDollar/MuSig2PartialSig");
    hasher << Params().GetConsensus().hashGenesisBlock;
    hasher << epoch;
    hasher << attempt_id;
    hasher << context_version;
    hasher << session_context_id;
    hasher << oracle_id;
    hasher << partial_sig;
    return hasher.GetHash();
}

bool OracleMusigPartialSigMsg::Sign(const CKey& key)
{
    uint256 hash = GetSignatureHash();
    signature.resize(64);
    if (!key.SignSchnorr(hash, signature, nullptr, uint256())) {
        signature.clear();
        return false;
    }
    return true;
}

bool OracleMusigPartialSigMsg::VerifySignature(const XOnlyPubKey& pubkey) const
{
    if (signature.size() != 64) return false;
    if (!pubkey.IsFullyValid()) return false;
    uint256 hash = GetSignatureHash();
    return pubkey.VerifySchnorr(hash, signature);
}

uint256 OracleMusigContextMsg::GetHash() const
{
    CHashWriter hasher(0);
    hasher << std::string("DigiDollar/MuSig2ContextMsg/v1");
    hasher << Params().GetConsensus().hashGenesisBlock;
    hasher << epoch;
    hasher << attempt_id;
    hasher << context_version;
    hasher << epoch_selection_seed;
    hasher << proposer_id;
    hasher << participant_ids;
    hasher << nonce_set_hash;
    hasher << quote_set_hash;
    hasher << consensus_price;
    hasher << consensus_timestamp;
    hasher << session_context_id;
    hasher << nonce_evidence;
    hasher << price_evidence;
    return hasher.GetHash();
}

uint256 OracleMusigContextMsg::GetSignatureHash() const
{
    CHashWriter hasher(0);
    hasher << std::string("DigiDollar/MuSig2ContextProposal");
    hasher << Params().GetConsensus().hashGenesisBlock;
    hasher << epoch;
    hasher << attempt_id;
    hasher << context_version;
    hasher << epoch_selection_seed;
    hasher << proposer_id;
    hasher << participant_ids;
    hasher << nonce_set_hash;
    hasher << quote_set_hash;
    hasher << consensus_price;
    hasher << consensus_timestamp;
    hasher << session_context_id;
    hasher << nonce_evidence;
    hasher << price_evidence;
    return hasher.GetHash();
}

bool OracleMusigContextMsg::Sign(const CKey& key)
{
    uint256 hash = GetSignatureHash();
    signature.resize(64);
    if (!key.SignSchnorr(hash, signature, nullptr, uint256())) {
        signature.clear();
        return false;
    }
    return true;
}

bool OracleMusigContextMsg::VerifySignature(const XOnlyPubKey& pubkey) const
{
    if (signature.size() != 64) return false;
    if (!pubkey.IsFullyValid()) return false;
    uint256 hash = GetSignatureHash();
    return pubkey.VerifySchnorr(hash, signature);
}

uint256 OracleVersionHeartbeatMsg::GetHash() const
{
    CHashWriter hasher(0);
    hasher << std::string("DigiDollar/OracleHeartbeatMsg/v1");
    hasher << GetSignatureHash();
    hasher << signature;
    return hasher.GetHash();
}

uint256 OracleVersionHeartbeatMsg::GetSignatureHash() const
{
    CHashWriter hasher(0);
    hasher << std::string("DigiDollar/OracleHeartbeat/v1");
    hasher << Params().GetConsensus().hashGenesisBlock;
    hasher << heartbeat_version;
    hasher << oracle_id;
    hasher << timestamp;
    hasher << nonce;
    hasher << client_version;
    hasher << p2p_protocol_version;
    hasher << oracle_protocol_version;
    hasher << musig2_context_version;
    hasher << software_version;
    hasher << subversion;
    return hasher.GetHash();
}

bool OracleVersionHeartbeatMsg::Sign(const CKey& key)
{
    const uint256 hash = GetSignatureHash();
    signature.resize(64);
    if (!key.SignSchnorr(hash, signature, nullptr, uint256())) {
        signature.clear();
        return false;
    }
    return true;
}

bool OracleVersionHeartbeatMsg::VerifySignature(const XOnlyPubKey& pubkey) const
{
    if (signature.size() != 64) return false;
    if (!pubkey.IsFullyValid()) return false;
    return pubkey.VerifySchnorr(GetSignatureHash(), signature);
}
