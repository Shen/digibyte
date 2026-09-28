// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see COPYING.
#include <algorithm>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <chrono>
#include <common/args.h>
#include <common/settings.h>
#include <fstream>
#include <hash.h>
#include <map>
#include <mutex>
#include <net.h>
#include <netbase.h>
#include <node/context.h>
#include <random.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <set>
#include <shutdown.h>
#include <sstream>
#include <thread>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/string.h>
#include <util/time.h>
#ifdef WIN32
#include <aclapi.h>
#include <windows.h>
#endif

namespace {
std::mutex g_config_mutex;
std::mutex g_probe_mutex;
int64_t g_last_probe{0}; // guarded by g_probe_mutex, monotonic milliseconds
constexpr size_t MAX_CONFIG_BYTES{1024 * 1024};
const std::set<std::string> CONFIG_KEYS{"digidollar", "paymaster", "prune", "txindex", "v2transport", "maxconnections", "paymastermaxoutbound", "paymastermaxinbound", "paymasterbind", "paymasterendpoint"};
struct ConfigPlan {
    fs::path path;
    bool existed{false};
    std::string original, replacement;
    std::vector<std::pair<fs::path, std::string>> inputs;
    UniValue result{UniValue::VOBJ};
};
std::string ReadConfig(const fs::path& path)
{
    if (!fs::exists(path)) return {};
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::file_size(path) > MAX_CONFIG_BYTES) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_UNSAFE_FILE");
    std::ifstream file{static_cast<const std::filesystem::path&>(path), std::ios::binary};
    if (!file) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_READ_FAILED");
    std::string data(MAX_CONFIG_BYTES + 1, '\0');
    file.read(data.data(), data.size());
    data.resize(static_cast<size_t>(file.gcount()));
    if (file.bad() || data.size() > MAX_CONFIG_BYTES || data.find('\0') != std::string::npos) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_READ_FAILED");
    return data;
}
struct ConfigEntry {
    std::string section, key, value;
    size_t line;
};
std::vector<ConfigEntry> ParseConfig(const std::string& data)
{
    std::vector<ConfigEntry> entries;
    std::istringstream input{data};
    std::string line, section;
    size_t number{0};
    while (std::getline(input, line)) {
        ++number;
        const auto parsed = TrimString(line.substr(0, line.find('#')));
        if (parsed.empty()) continue;
        if (parsed.front() == '[' && parsed.back() == ']') {
            section = parsed.substr(1, parsed.size() - 2);
            continue;
        }
        const auto separator = parsed.find('=');
        if (separator == std::string::npos || parsed.front() == '-') throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_PARSE_ERROR");
        // Match ArgsManager: a dotted key inside a section retains that section prefix.
        std::string full = (section.empty() ? "" : section + ".") + TrimString(parsed.substr(0, separator));
        const auto dot = full.rfind('.');
        entries.push_back({dot == std::string::npos ? "" : full.substr(0, dot), dot == std::string::npos ? full : full.substr(dot + 1), TrimString(parsed.substr(separator + 1)), number - 1});
    }
    return entries;
}
bool ProbeAddress(const CService& endpoint)
{
    return endpoint.IsValid() && endpoint.GetPort() && (endpoint.IsIPv4() || endpoint.IsIPv6() || endpoint.IsTor()) &&
           (endpoint.IsRoutable() || (Params().GetChainType() == ChainType::REGTEST && endpoint.IsLocal()));
}
ConfigPlan PrepareConfig(ArgsManager& args, const UniValue& requested)
{
    if (!requested.isObject() || requested.empty() || requested.size() > CONFIG_KEYS.size()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Supply a non-empty settings object containing only Paymaster node options");
    const std::string network = ChainTypeToString(Params().GetChainType());
    ConfigPlan plan;
    plan.path = args.GetConfigFilePath();
    plan.existed = fs::exists(plan.path);
    plan.original = ReadConfig(plan.path);
    std::map<std::string, std::vector<std::string>> desired;
    for (const auto& key : requested.getKeys()) {
        if (!CONFIG_KEYS.count(key) || desired.count(key)) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_UNKNOWN_OR_DUPLICATE_KEY: " + key);
        const auto& value = requested.find_value(key);
        if (key == "paymasterbind") {
            if (!value.isArray() || value.empty() || value.size() > 8) throw JSONRPCError(RPC_INVALID_PARAMETER, "paymasterbind requires 1..8 addresses");
            std::set<std::string> endpoints;
            for (const auto& entry : value.getValues()) {
                std::string bind = entry.get_str();
                const bool onion = bind.size() > 6 && bind.substr(bind.size() - 6) == "=onion";
                const auto address = LookupNumeric(onion ? bind.substr(0, bind.size() - 6) : bind, 0);
                if (!address.IsValid() || !address.GetPort() || (!address.IsIPv4() && !address.IsIPv6()) || (onion && !address.IsLocal()) || !endpoints.insert(address.ToStringAddrPort()).second) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_INVALID_BIND");
                if (address.GetPort() == args.GetIntArg("-port", Params().GetDefaultPort()) || address.GetPort() == args.GetIntArg("-rpcport", BaseParams().RPCPort())) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_PORT_CONFLICT: use a dedicated Direct port");
                for (const auto& name : {"-bind", "-whitebind", "-rpcbind"})
                    for (auto occupied : args.GetArgs(name)) {
                        if (std::string{name} == "-bind" && occupied.size() > 6 && occupied.substr(occupied.size() - 6) == "=onion")
                            occupied.resize(occupied.size() - 6);
                        else if (std::string{name} == "-whitebind") {
                            const auto equals = occupied.find('=');
                            if (equals != std::string::npos) occupied = occupied.substr(equals + 1);
                        }
                        const auto listener = LookupNumeric(occupied, 0);
                        if (listener.GetPort() && listener.GetPort() == address.GetPort()) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_PORT_CONFLICT: " + std::string{name});
                    }
                desired[key].push_back(address.ToStringAddrPort() + (onion ? "=onion" : ""));
            }
        } else if (key == "paymasterendpoint") {
            const auto endpoint = LookupNumeric(value.get_str(), 0);
            if (!ProbeAddress(endpoint)) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_INVALID_ENDPOINT");
            desired[key] = {endpoint.ToStringAddrPort()};
        } else {
            if (!value.isNum() && !value.isBool()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Numeric or boolean setting required: " + key);
            const int64_t number = value.isBool() ? int64_t(value.get_bool()) : value.getInt<int64_t>();
            const int64_t maximum = key == "maxconnections" ? 1000000 : key == "paymastermaxinbound" ? 16 :
                                                                    key == "paymastermaxoutbound"    ? 4 :
                                                                    key == "prune"                   ? 0 :
                                                                                                       1;
            if (number < 0 || number > maximum) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_OUT_OF_RANGE: " + key);
            desired[key] = {std::to_string(number)};
        }
    }
    const auto conflict = [&](const std::string& key, const std::string& source) { throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_OVERRIDE: " + key + " (" + source + ")"); };
    args.LockSettings([&](const common::Settings& settings) {
        for (const auto& [key, values] : desired) {
            if (settings.forced_settings.count(key)) conflict(key, "forced setting");
            if (settings.command_line_options.count(key)) conflict(key, "command line");
            if (settings.rw_settings.count(key)) conflict(key, "settings.json");
        }
    });
    // Bind the preview to disk settings as well as already loaded settings.
    fs::path settings_path;
    if (args.GetSettingsPath(&settings_path)) {
        const auto data = ReadConfig(settings_path);
        plan.inputs.emplace_back(settings_path, data);
        UniValue settings;
        if (fs::exists(settings_path) && (!settings.read(data) || !settings.isObject())) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_SETTINGS_INVALID");
        for (const auto& [key, values] : desired)
            for (const auto& candidate : {key, "no" + key, network + "." + key, network + ".no" + key}) {
                if (!settings.find_value(candidate).isNull()) conflict(key, fs::PathToString(settings_path));
            }
    }
    const auto entries = ParseConfig(plan.original);
    bool includes_disabled{false};
    args.LockSettings([&](const common::Settings& settings) { includes_disabled = settings.command_line_options.count("includeconf"); });
    size_t include_count{0};
    for (const auto& entry : entries) {
        if (includes_disabled || (!entry.section.empty() && entry.section != network)) continue;
        if (entry.key == "noincludeconf") conflict("includeconf", "negated include; resolve manually");
        if (entry.key != "includeconf") continue;
        if (++include_count > 16) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_TOO_MANY_INCLUDES");
        const auto path = AbsPathForConfigVal(args, fs::PathFromString(entry.value), /*net_specific=*/false);
        if (!fs::exists(path)) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_INCLUDED_FILE_MISSING: " + fs::PathToString(path));
        const auto data = ReadConfig(path);
        plan.inputs.emplace_back(path, data);
        for (const auto& included : ParseConfig(data)) {
            if (!included.section.empty() && included.section != network) continue;
            const auto key = included.key.substr(0, 2) == "no" ? included.key.substr(2) : included.key;
            if (desired.count(key)) conflict(key, "included file " + fs::PathToString(path));
        }
    }
    std::vector<std::string> lines;
    std::istringstream input{plan.original};
    std::string line;
    while (std::getline(input, line))
        lines.push_back(line);
    std::map<std::string, std::vector<size_t>> replace;
    std::map<std::string, size_t> defaults;
    for (const auto& entry : entries) {
        if (!entry.section.empty() && entry.section != network) continue;
        const bool negated = entry.key.substr(0, 2) == "no";
        const auto key = negated ? entry.key.substr(2) : entry.key;
        if (!desired.count(key)) continue;
        // Network-only default entries are ignored by Core outside mainnet.
        if (entry.section.empty() && network != "main" && (key == "paymasterbind" || key == "paymasterendpoint")) continue;
        if (negated) conflict(key, "negated configuration entry");
        if (entry.section.empty()) {
            if (++defaults[key] > 1) conflict(key, "duplicate default-section entries");
            // List options merge across sections. Do not leave a second active bind behind.
            if (key == "paymasterbind") conflict(key, "move default-section binds to [main] before applying");
        } else
            replace[key].push_back(entry.line);
    }
    UniValue changes{UniValue::VARR}, canonical{UniValue::VOBJ};
    std::set<size_t> removed;
    std::vector<std::string> append;
    for (const auto& [key, values] : desired) {
        if (replace[key].size() > 1 && key != "paymasterbind") conflict(key, "duplicate configuration entries");
        std::string text;
        for (const auto& value : values)
            text += key + "=" + value + "\n";
        if (replace[key].empty())
            append.push_back(text);
        else {
            const size_t index = replace[key].front();
            // Keep an existing inline comment as a separate line.
            const auto comment = lines[index].find('#');
            const std::string note = comment == std::string::npos ? "" : lines[index].substr(comment) + "\n";
            // Dotted entries keep their explicit section regardless of surrounding headers.
            const auto parsed_key = TrimString(lines[index].substr(0, lines[index].find('=')));
            if (parsed_key.find('.') != std::string::npos) {
                text.clear();
                for (const auto& value : values)
                    text += network + "." + key + "=" + value + "\n";
            }
            lines[index] = note + text.substr(0, text.size() - 1);
            for (size_t n = 1; n < replace[key].size(); ++n) {
                const auto extra = replace[key][n];
                const auto marker = lines[extra].find('#');
                if (marker == std::string::npos)
                    removed.insert(extra);
                else
                    lines[extra] = lines[extra].substr(marker);
            }
        }
        UniValue after;
        if (key == "paymasterbind") {
            after = UniValue{UniValue::VARR};
            for (const auto& value : values)
                after.push_back(value);
        } else if (key == "paymasterendpoint")
            after = UniValue{values.front()};
        else
            after = UniValue{UniValue::VNUM, values.front()};
        canonical.pushKV(key, after);
        UniValue change{UniValue::VOBJ};
        change.pushKV("option", key);
        change.pushKV("effective_before", args.GetArg("-" + key, "(default)"));
        change.pushKV("after_restart", after);
        changes.push_back(change);
    }
    for (size_t index = 0; index < lines.size(); ++index)
        if (!removed.count(index)) plan.replacement += lines[index] + "\n";
    if (!append.empty()) {
        plan.replacement += "\n# Paymaster operator setup\n[" + network + "]\n";
        for (const auto& entry : append)
            plan.replacement += entry;
    }
    if (plan.replacement.size() > MAX_CONFIG_BYTES) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_TOO_LARGE");
    HashWriter hash;
    hash << network << fs::PathToString(plan.path) << plan.existed << plan.original << plan.replacement;
    for (const auto& [path, data] : plan.inputs)
        hash << fs::PathToString(path) << data;
    plan.result.pushKV("plan_id", hash.GetHash().GetHex());
    plan.result.pushKV("network", network);
    plan.result.pushKV("target", fs::PathToString(plan.path));
    plan.result.pushKV("settings", canonical);
    plan.result.pushKV("changes", changes);
    plan.result.pushKV("restart_required", true);
    plan.result.pushKV("external_reachability", "unknown");
    plan.result.pushKV("instructions", "Configure firewall/NAT or Tor HiddenServicePort to the dedicated Direct bind. Restart the node explicitly, then recheck getpaymasteroperatorinfo. Disabling pruning may require downloading old blocks again; no reindex or restart is performed here.");
    return plan;
}
void CopyPermissions(const fs::path& original, const fs::path& destination)
{
#ifdef WIN32
    DWORD length{0};
    GetFileSecurityW(original.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &length);
    if (!length) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_PERMISSIONS_FAILED");
    std::vector<unsigned char> descriptor(length);
    auto* security = reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data());
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{0};
    if (!GetFileSecurityW(original.c_str(), DACL_SECURITY_INFORMATION, security, length, &length) || !GetSecurityDescriptorControl(security, &control, &revision)) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_PERMISSIONS_FAILED");
    const SECURITY_INFORMATION inheritance = control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
    if (!SetFileSecurityW(destination.c_str(), DACL_SECURITY_INFORMATION | inheritance, security)) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_PERMISSIONS_FAILED");
#else
    fs::permissions(destination, fs::status(original).permissions());
#endif
}
void RestrictNewFile(const fs::path& path)
{
#ifdef WIN32
    HANDLE token{nullptr};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_PERMISSIONS_FAILED");
    DWORD length{0};
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    std::vector<unsigned char> user(length);
    const bool read = length && GetTokenInformation(token, TokenUser, user.data(), length, &length);
    CloseHandle(token);
    if (!read) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_PERMISSIONS_FAILED");
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
    PACL acl{nullptr};
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_PERMISSIONS_FAILED");
    const auto result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    if (result != ERROR_SUCCESS) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_PERMISSIONS_FAILED");
#else
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
#endif
}
void WritePrivate(const fs::path& path, const std::string& data, const fs::path& permissions_source)
{
    if (fs::exists(path)) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_FILE_EXISTS");
    FILE* file = fsbridge::fopen(path, "wbx");
    if (!file) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_WRITE_FAILED");
    try {
        RestrictNewFile(path);
        if (!permissions_source.empty()) CopyPermissions(permissions_source, path);
        const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size() && FileCommit(file);
        const bool closed = std::fclose(file) == 0;
        file = nullptr;
        if (!written || !closed) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_WRITE_FAILED");
    } catch (...) {
        if (file) std::fclose(file);
        fs::remove(path);
        throw;
    }
}
std::vector<RPCArg> NodeSettingsArgs()
{
    std::vector<RPCArg> fields;
    for (const auto& key : CONFIG_KEYS) {
        if (key == "paymasterbind")
            fields.emplace_back(key, RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Dedicated Direct binds", std::vector<RPCArg>{{"bind", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Numeric IP:port, optionally =onion"}});
        else
            fields.emplace_back(key, key == "paymasterendpoint" ? RPCArg::Type::STR : RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Startup setting; see node help");
    }
    return fields;
}
RPCHelpMan preparepaymasternodeconfig()
{
    return RPCHelpMan{"preparepaymasternodeconfig", "Preview restart-only changes to this node's active configuration. Does not write or probe. Conflicting overrides, ambiguous entries and conflicting included files require manual resolution.\n", {{"settings", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Allowed Paymaster node options", NodeSettingsArgs()}}, RPCResult{RPCResult::Type::OBJ, "", "Reviewable config plan", {{RPCResult::Type::ELISION, "", "plan_id, network, target, settings, changes, restart_required, external_reachability, instructions"}}}, RPCExamples{HelpExampleCli("preparepaymasternodeconfig", "'{\"paymastermaxoutbound\":1}'")}, [](const RPCHelpMan&, const JSONRPCRequest& request) {
                          std::lock_guard<std::mutex> lock(g_config_mutex);
                          return PrepareConfig(*EnsureAnyNodeContext(request.context).args, request.params[0]).result;
                      }};
}
RPCHelpMan applypaymasternodeconfig()
{
    return RPCHelpMan{"applypaymasternodeconfig", "Apply an unchanged config plan after explicit operator review. Creates a backup and atomically replaces the active config. Does not restart the node or change running settings.\n", {{"plan", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Reviewed plan binding", {{"plan_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Preview plan id"}, {"settings", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Unchanged preview settings", NodeSettingsArgs()}}}}, RPCResult{RPCResult::Type::OBJ, "", "Applied configuration", {{RPCResult::Type::BOOL, "applied", "Written successfully"}, {RPCResult::Type::BOOL, "restart_required", "Explicit restart needed"}, {RPCResult::Type::STR, "backup", "Original config backup, empty for a new config"}}}, RPCExamples{HelpExampleCli("applypaymasternodeconfig", "'{}'")}, [](const RPCHelpMan&, const JSONRPCRequest& request) {
                          std::lock_guard<std::mutex> lock(g_config_mutex);
                          const auto& supplied = request.params[0].get_obj();
                          RPCTypeCheckObj(supplied, {{"plan_id", UniValueType(UniValue::VSTR)}, {"settings", UniValueType(UniValue::VOBJ)}}, false, true);
                          auto plan = PrepareConfig(*EnsureAnyNodeContext(request.context).args, supplied.find_value("settings"));
                          if (plan.result.find_value("plan_id").get_str() != supplied.find_value("plan_id").get_str()) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_PLAN_CHANGED");
                          const auto suffix = GetRandHash().GetHex();
                          const fs::path temporary = fs::PathFromString(fs::PathToString(plan.path) + ".paymaster-" + suffix + ".tmp");
                          const fs::path backup = fs::PathFromString(fs::PathToString(plan.path) + ".paymaster-" + suffix + ".bak");
                          try {
                              if (plan.existed) WritePrivate(backup, plan.original, {});
                              WritePrivate(temporary, plan.replacement, plan.existed ? plan.path : fs::path{});
                              if (PrepareConfig(*EnsureAnyNodeContext(request.context).args, supplied.find_value("settings")).result.find_value("plan_id").get_str() != plan.result.find_value("plan_id").get_str()) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_CONFIG_PLAN_CHANGED");
                              if (!RenameOver(temporary, plan.path)) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_CONFIG_RENAME_FAILED");
                              DirectoryCommit(plan.path.parent_path());
                          } catch (...) {
                              if (fs::exists(temporary)) fs::remove(temporary);
                              throw;
                          }
                          UniValue result{UniValue::VOBJ};
                          result.pushKV("applied", true);
                          result.pushKV("restart_required", true);
                          result.pushKV("backup", plan.existed ? fs::PathToString(backup) : "");
                          return result;
                      }};
}
RPCHelpMan checkpaymasterendpoint()
{
    return RPCHelpMan{"checkpaymasterendpoint", "Explicit bounded v2/Paymaster transport check from this node. No payment, capacity reservation, identity authentication or guarantee of availability. At most one probe per node and one start per 30 seconds.\n", {{"endpoint", RPCArg::Type::STR, RPCArg::Optional::NO, "Numeric public IP:port or onion:port; loopback only on regtest"}}, RPCResult{RPCResult::Type::OBJ, "", "Transport observation", {{RPCResult::Type::STR, "network", "Probe network"}, {RPCResult::Type::STR, "endpoint", "Probed endpoint"}, {RPCResult::Type::NUM_TIME, "observed_at", "Probe time"}, {RPCResult::Type::BOOL, "transport_ready", "v2 and Paymaster negotiated"}, {RPCResult::Type::STR, "state", "Terminal transport state"}, {RPCResult::Type::OBJ, "stages", "Only positively observed stages are marked passed", {{RPCResult::Type::STR, "v2_transport", "passed or unknown"}, {RPCResult::Type::STR, "paymaster_negotiation", "passed or unknown"}}}, {RPCResult::Type::BOOL, "payment_verified", "Always false"}, {RPCResult::Type::BOOL, "identity_verified", "Always false"}}}, RPCExamples{HelpExampleCli("checkpaymasterendpoint", "\"PUBLIC_IP:12033\"")}, [](const RPCHelpMan&, const JSONRPCRequest& request) {
                          using namespace DigiDollar::Paymaster;
                          auto& node = EnsureAnyNodeContext(request.context);
                          if (!node.connman) throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "P2P unavailable");
                          const auto endpoint = LookupNumeric(request.params[0].get_str(), 0);
                          if (!ProbeAddress(endpoint)) throw JSONRPCError(RPC_INVALID_PARAMETER, "PAYMASTER_DIAGNOSTIC_ENDPOINT_INVALID");
                          std::unique_lock<std::mutex> lock(g_probe_mutex, std::try_to_lock);
                          if (!lock.owns_lock()) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_DIAGNOSTIC_BUSY");
                          const auto start = DirectNow();
                          if (g_last_probe && start - g_last_probe < 30000) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_DIAGNOSTIC_RATE_LIMIT");
                          if (node.connman->GetPaymasterBudget().outbound <= node.connman->GetPaymasterUsage().outgoing || node.connman->GetPaymasterQueueSize()) throw JSONRPCError(RPC_MISC_ERROR, "PAYMASTER_DIAGNOSTIC_CAPACITY_BUSY");
                          g_last_probe = start;
                          const DirectKey key{GetRandHash().GetHex(), "operator-diagnostic", false};
                          struct Release {
                              CConnman& connman;
                              DirectKey key;
                              ~Release() { connman.ReleasePaymasterConnection(key); }
                          } release{*node.connman, key};
                          DirectState state{DirectState::EXPIRED};
                          while (!ShutdownRequested() && DirectNow() - start < DIRECT_HANDSHAKE_MS) {
                              auto result = node.connman->RequestPaymasterConnection(key, endpoint, endpoint.IsTor());
                              state = result.state;
                              if (state != DirectState::QUEUED && state != DirectState::CONNECTING && state != DirectState::HANDSHAKING) break;
                              std::this_thread::sleep_for(std::chrono::milliseconds{50});
                          }
                          if (state == DirectState::QUEUED || state == DirectState::CONNECTING || state == DirectState::HANDSHAKING) state = DirectState::EXPIRED;
                          UniValue result{UniValue::VOBJ};
                          result.pushKV("network", ChainTypeToString(Params().GetChainType()));
                          result.pushKV("endpoint", endpoint.ToStringAddrPort());
                          result.pushKV("observed_at", GetTime());
                          result.pushKV("transport_ready", state == DirectState::READY);
                          result.pushKV("state", DirectStateName(state));
                          UniValue stages{UniValue::VOBJ};
                          stages.pushKV("v2_transport", state == DirectState::READY ? "passed" : "unknown");
                          stages.pushKV("paymaster_negotiation", state == DirectState::READY ? "passed" : "unknown");
                          result.pushKV("stages", stages);
                          result.pushKV("payment_verified", false);
                          result.pushKV("identity_verified", false);
                          return result;
                      }};
}
} // namespace
void RegisterPaymasterRPCCommands(CRPCTable& table)
{
    static const CRPCCommand commands[]{
        {"digidollar", &preparepaymasternodeconfig},
        {"digidollar", &applypaymasternodeconfig},
        {"digidollar", &checkpaymasterendpoint},
    };
    for (const auto& command : commands)
        table.appendCommand(command.name, &command);
}
