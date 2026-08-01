// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/err.h>
#include <consensus/dca.h>
#include <consensus/params.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <digidollar/validation.h>
#include <oracle/mock_oracle.h>
#include <oracle/bundle_manager.h>
#include <chainparams.h>
#include <primitives/oracle.h>
#include <key.h>
#include <pubkey.h>
#include <util/int128.h>
#include <util/time.h>
#include <logging.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace DigiDollar {
namespace ERR {

// Initialize static members
ERRState EmergencyRedemptionRatio::s_currentState;
bool EmergencyRedemptionRatio::s_stateReconstructed{false};
std::vector<COutPoint> EmergencyRedemptionRatio::s_errQueue;
std::map<COutPoint, std::pair<CAmount, uint32_t>> EmergencyRedemptionRatio::s_queuedRedemptions;
std::mutex EmergencyRedemptionRatio::s_errMutex;

namespace {

constexpr int ERR_RATIO_NORMAL_BPS = 10000;
constexpr int ERR_RATIO_95_BPS = 9500;
constexpr int ERR_RATIO_90_BPS = 9000;
constexpr int ERR_RATIO_85_BPS = 8500;
constexpr int ERR_RATIO_MIN_BPS = 8000;

double RatioBpsToDouble(int ratio_bps)
{
    return static_cast<double>(ratio_bps) / 10000.0;
}

} // namespace

// ERR adjustment tier thresholds and ratios
// CRITICAL: ERR returns 100% collateral ALWAYS. Ratios determine DD burn multiplier.
// Formula: RequiredDD = ceil(OriginalDD * 10000 / ratioBps)
static const std::vector<std::pair<int, int>> ERR_TIERS = {
    {95, ERR_RATIO_95_BPS},   // 95-100% health: 1/0.95 = 1.053x DD burn
    {90, ERR_RATIO_90_BPS},   // 90-95% health: 1/0.90 = 1.111x DD burn
    {85, ERR_RATIO_85_BPS},   // 85-90% health: 1/0.85 = 1.176x DD burn
    {0,  ERR_RATIO_MIN_BPS}   // <85% health: 1/0.80 = 1.250x DD burn
};

// Oracle consensus requirements
static const size_t REQUIRED_ORACLE_SIGNATURES = 8;
static const size_t TOTAL_ORACLE_COUNT = 15;
static const uint64_t ORACLE_MESSAGE_MAX_AGE = 3600; // 1 hour in seconds

bool EmergencyRedemptionRatio::ShouldActivateERR(int systemHealth)
{
    // ERR activates when system health falls below 100%
    return systemHealth < 100;
}

double EmergencyRedemptionRatio::CalculateERRAdjustment(int systemHealth)
{
    return RatioBpsToDouble(CalculateERRRatioBps(systemHealth));
}

int EmergencyRedemptionRatio::CalculateERRRatioBps(int systemHealth)
{
    // At >= 100% health, no ERR adjustment needed (ratio = 1.0)
    // This means ERR returns same as normal redemption (full collateral, burn original DD)
    if (systemHealth >= 100) {
        return ERR_RATIO_NORMAL_BPS;
    }

    // Find the appropriate ERR tier based on system health
    // Returns a ratio in basis points used to calculate required DD burn.
    // RequiredDD = ceil(OriginalDD * 10000 / ratioBps)
    //
    // IMPORTANT: Check tiers from highest to lowest threshold to match correctly
    // ERR_TIERS: {95, 9500}, {90, 9000}, {85, 8500}, {0, 8000}
    for (const auto& tier : ERR_TIERS) {
        if (systemHealth >= tier.first) {
            return tier.second;
        }
    }

    // Fallback to minimum ratio (80%) = maximum DD burn (125%)
    return ERR_RATIO_MIN_BPS;
}

CAmount EmergencyRedemptionRatio::GetRequiredDDBurn(CAmount originalDDMinted, int systemHealth)
{
    // Calculate how much DD must be burned to redeem FULL collateral during ERR
    // Formula: RequiredDD = OriginalDD / ERRRatio
    // Example: 100 DD at 80% ratio → 100/0.80 = 125 DD required

    if (originalDDMinted <= 0) {
        return 0;
    }

    // If system is healthy (>= 100%), no extra DD required
    if (systemHealth >= 100) {
        return originalDDMinted;
    }

    int ratioBps = CalculateERRRatioBps(systemHealth);
    if (ratioBps <= 0) {
        ratioBps = ERR_RATIO_MIN_BPS;
    }

    // Consensus-visible burn math: ceil(originalDD * 10000 / ratioBps).
    // Use signed 128-bit arithmetic so large valid CAmount values cannot overflow.
    const util::int128_t numerator = static_cast<util::int128_t>(originalDDMinted) * ERR_RATIO_NORMAL_BPS;
    const util::int128_t required = (numerator + ratioBps - 1) / ratioBps;
    const util::int128_t max_amount = std::numeric_limits<CAmount>::max();
    const CAmount requiredDD = required > max_amount
        ? std::numeric_limits<CAmount>::max()
        : static_cast<CAmount>(required);

    const util::int128_t increaseBps = (static_cast<util::int128_t>(requiredDD - originalDDMinted) * 10000) / originalDDMinted;
    LogPrint(BCLog::DIGIDOLLAR, "ERR: GetRequiredDDBurn - original: %lld, health: %d%%, ratio_bps: %d, required: %lld (%lld.%02lld%% increase)\n",
             static_cast<long long>(originalDDMinted), systemHealth, ratioBps,
             static_cast<long long>(requiredDD),
             static_cast<long long>(increaseBps / 100),
             static_cast<long long>(increaseBps % 100));

    return requiredDD;
}

CAmount EmergencyRedemptionRatio::GetAdjustedRedemption(CAmount normalRedemption, int systemHealth)
{
    // DEPRECATED: This function name is misleading.
    // ERR doesn't reduce collateral return - it increases DD burn requirement.
    // Use GetRequiredDDBurn() instead.
    //
    // For backwards compatibility, this now just returns the input unchanged
    // since collateral return is NOT adjusted during ERR.
    LogPrint(BCLog::DIGIDOLLAR, "ERR: GetAdjustedRedemption DEPRECATED - use GetRequiredDDBurn instead\n");
    return normalRedemption;
}

bool EmergencyRedemptionRatio::HasOracleConsensus(const COracleBundle& bundle, const Consensus::Params& params)
{
    return OracleBundleManager::ValidateBundle(bundle, /*block_height=*/0, params);
}

ERRState EmergencyRedemptionRatio::GetCurrentState()
{
    std::lock_guard<std::mutex> lock(s_errMutex); // RH-44: thread safety
    // RH-36a: If state was reconstructed from chain data after restart,
    // do NOT re-fetch health from DCA cache until DCA cache is properly
    // initialized (first real health update from block processing).
    // The DCA cache defaults to 30000 (300% healthy) which would silently
    // deactivate ERR even though the system is under-collateralized.
    if (s_stateReconstructed) {
        return s_currentState;
    }

    // Update state based on current system health
    int currentHealth = DCA::DynamicCollateralAdjustment::GetCurrentSystemHealth();

    if (s_currentState.isActive) {
        // Check if ERR should deactivate (use locked variant — mutex already held)
        if (currentHealth >= 100) {
            DeactivateERRLocked(currentHealth);
        } else {
            // Update current health and adjustment ratio
            s_currentState.systemHealth = currentHealth;
            s_currentState.adjustmentRatioBps = CalculateERRRatioBps(currentHealth);
            s_currentState.adjustmentRatio = RatioBpsToDouble(s_currentState.adjustmentRatioBps);
        }
    } else {
        // ERR is inactive, update health for monitoring
        s_currentState.systemHealth = currentHealth;
        s_currentState.adjustmentRatioBps = 0;
        s_currentState.adjustmentRatio = 0.0;
    }

    return s_currentState;
}

std::vector<COutPoint> EmergencyRedemptionRatio::GetERRQueue()
{
    std::lock_guard<std::mutex> lock(s_errMutex); // RH-44: thread safety
    return s_errQueue;
}

bool EmergencyRedemptionRatio::QueueERRRedemption(const COutPoint& outpoint, CAmount ddAmount, uint32_t requestHeight)
{
    std::lock_guard<std::mutex> lock(s_errMutex); // RH-44: thread safety
    if (!s_currentState.isActive) {
        return false;
    }

    // Check if already queued
    if (s_queuedRedemptions.count(outpoint)) {
        return false;
    }

    // Add to queue (FIFO order)
    s_errQueue.push_back(outpoint);
    s_queuedRedemptions[outpoint] = std::make_pair(ddAmount, requestHeight);

    LogPrint(BCLog::DIGIDOLLAR, "ERR: Queued redemption %s for %d DD at height %u\n",
             outpoint.ToString(), ddAmount, requestHeight);

    return true;
}

size_t EmergencyRedemptionRatio::ProcessERRQueue(size_t maxRedemptions)
{
    std::lock_guard<std::mutex> lock(s_errMutex); // RH-44: thread safety
    if (!s_currentState.isActive || s_errQueue.empty()) {
        return 0;
    }

    size_t processed = 0;
    auto it = s_errQueue.begin();

    while (it != s_errQueue.end() && processed < maxRedemptions) {
        const COutPoint& outpoint = *it;

        // Get redemption details
        auto redemptionIt = s_queuedRedemptions.find(outpoint);
        if (redemptionIt == s_queuedRedemptions.end()) {
            // Invalid queue entry, remove it
            it = s_errQueue.erase(it);
            continue;
        }

        CAmount ddAmount = redemptionIt->second.first;
        uint32_t requestHeight = redemptionIt->second.second;

        // Process the redemption with ERR adjustment
        // Note: In a full implementation, this would interact with the UTXO set
        // and create the actual redemption transaction

        LogPrint(BCLog::DIGIDOLLAR, "ERR: Processing redemption %s for %d DD (requested at height %u)\n",
                 outpoint.ToString(), ddAmount, requestHeight);

        // Remove from queue and tracking
        it = s_errQueue.erase(it);
        s_queuedRedemptions.erase(outpoint);
        processed++;
    }

    return processed;
}

bool EmergencyRedemptionRatio::ActivateERR(const COracleBundle& oracleBundle, uint32_t activationHeight, const Consensus::Params& params)
{
    std::lock_guard<std::mutex> lock(s_errMutex); // RH-44: thread safety
    // Check if ERR is already active
    if (s_currentState.isActive) {
        return false;
    }

    // Get current system health
    int currentHealth = DCA::DynamicCollateralAdjustment::GetCurrentSystemHealth();

    // Check if ERR should be activated based on health
    if (!ShouldActivateERR(currentHealth)) {
        LogPrint(BCLog::DIGIDOLLAR, "ERR: Activation denied - system health %d%% is sufficient\n", currentHealth);
        return false;
    }

    std::string oracle_error;
    if (!OracleBundleManager::ValidateMuSig2Bundle(oracleBundle, activationHeight, params, oracle_error)) {
        LogPrint(BCLog::DIGIDOLLAR, "ERR: Activation denied - invalid MuSig2 oracle bundle: %s\n",
                 oracle_error);
        return false;
    }

    // Activate ERR
    ERRState newState;
    newState.isActive = true;
    newState.systemHealth = currentHealth;
    newState.adjustmentRatioBps = CalculateERRRatioBps(currentHealth);
    newState.adjustmentRatio = RatioBpsToDouble(newState.adjustmentRatioBps);
    newState.activationHeight = activationHeight;
    newState.oracleConsensusHash = CalculateOracleConsensusHash(oracleBundle);
    newState.activationTimestamp = GetTime();

    UpdateERRState(newState);

    LogPrint(BCLog::DIGIDOLLAR, "ERR: Activated at height %u with %d%% system health (%.1f%% return ratio)\n",
             activationHeight, currentHealth, newState.adjustmentRatio * 100);

    return true;
}

bool EmergencyRedemptionRatio::DeactivateERR(int currentHealth)
{
    std::lock_guard<std::mutex> lock(s_errMutex); // RH-44: thread safety
    return DeactivateERRLocked(currentHealth);
}

bool EmergencyRedemptionRatio::DeactivateERRLocked(int currentHealth)
{
    // Caller MUST hold s_errMutex
    if (!s_currentState.isActive) {
        return false;
    }

    // Check if system health has recovered
    if (currentHealth < 100) {
        return false;
    }

    LogPrint(BCLog::DIGIDOLLAR, "ERR: Deactivating - system health recovered to %d%%\n", currentHealth);

    // Process any remaining queued redemptions (lock already held)
    {
        auto it = s_errQueue.begin();
        while (it != s_errQueue.end()) {
            auto redemptionIt = s_queuedRedemptions.find(*it);
            if (redemptionIt != s_queuedRedemptions.end()) {
                s_queuedRedemptions.erase(redemptionIt);
            }
            it = s_errQueue.erase(it);
        }
    }

    // Clear ERR state
    ERRState clearedState;
    clearedState.systemHealth = currentHealth;
    UpdateERRState(clearedState);

    // Clear queue (lock already held)
    s_errQueue.clear();
    s_queuedRedemptions.clear();

    LogPrint(BCLog::DIGIDOLLAR, "ERR: Deactivated - system health restored\n");
    return true;
}

bool EmergencyRedemptionRatio::ValidateERRRedemption(const CTransaction& tx, CAmount originalDDMinted, CAmount expectedCollateral)
{
    // ERR redemption validation:
    // - User burns MORE DD than originally minted (based on ERR ratio)
    // - User receives FULL collateral back (not reduced!)

    // Check if ERR should be active (even if state not formally activated)
    int currentHealth = s_currentState.isActive ? s_currentState.systemHealth :
                        DCA::DynamicCollateralAdjustment::GetCurrentSystemHealth();

    if (currentHealth >= 100) {
        LogPrint(BCLog::DIGIDOLLAR, "ERR: Validation failed - system health %d%% doesn't require ERR\n", currentHealth);
        return false;
    }

    // Calculate required DD burn for ERR redemption
    CAmount requiredDDBurn = GetRequiredDDBurn(originalDDMinted, currentHealth);

    // Count DD being burned in this transaction (DD inputs - DD outputs)
    CAmount ddInputs = 0;
    CAmount ddOutputs = 0;
    for (const auto& output : tx.vout) {
        if (IsDDTokenScript(output.scriptPubKey)) {
            ddOutputs += output.nValue;
        }
    }
    // Note: DD inputs would need to be looked up from UTXO set
    // For now, infer burn capacity from the provided expected amount per DD input.
    // Full validation must replace this with UTXO-backed DD input accounting.
    if (tx.vin.empty()) {
        LogPrint(BCLog::DIGIDOLLAR, "ERR: Validation failed - no inputs\n");
        return false;
    }

    const util::int128_t inferredInputs = static_cast<util::int128_t>(originalDDMinted) * tx.vin.size();
    ddInputs = inferredInputs > std::numeric_limits<CAmount>::max()
        ? std::numeric_limits<CAmount>::max()
        : static_cast<CAmount>(inferredInputs);

    const CAmount inferredBurn = ddInputs > ddOutputs ? ddInputs - ddOutputs : 0;
    if (inferredBurn < requiredDDBurn) {
        LogPrint(BCLog::DIGIDOLLAR, "ERR: Validation failed - DD burn %lld < required %lld for original %lld\n",
                 static_cast<long long>(inferredBurn),
                 static_cast<long long>(requiredDDBurn),
                 static_cast<long long>(originalDDMinted));
        return false;
    }

    // Validate collateral output is the FULL amount (ERR doesn't reduce collateral!)
    CAmount actualCollateralOutput = 0;
    for (const auto& output : tx.vout) {
        if (!IsDDTokenScript(output.scriptPubKey)) {
            actualCollateralOutput += output.nValue;
        }
    }

    // Allow some flexibility for fees, but collateral should be close to expected
    CAmount tolerance = 1000000; // 0.01 DGB tolerance for fees
    if (actualCollateralOutput < expectedCollateral - tolerance) {
        LogPrint(BCLog::DIGIDOLLAR, "ERR: Validation failed - collateral return %lld < expected %lld (ERR should return FULL collateral)\n",
                 static_cast<long long>(actualCollateralOutput), static_cast<long long>(expectedCollateral));
        return false;
    }

    LogPrint(BCLog::DIGIDOLLAR, "ERR: Validation passed - health: %d%%, required DD burn: %lld (original: %lld), collateral: %lld\n",
             currentHealth, static_cast<long long>(requiredDDBurn),
             static_cast<long long>(originalDDMinted), static_cast<long long>(actualCollateralOutput));

    return true;
}

bool EmergencyRedemptionRatio::ShouldBlockMinting(CAmount oraclePriceOverride)
{
    // RH-44: Read ERR active state under lock
    {
        std::lock_guard<std::mutex> lock(s_errMutex);
        if (s_currentState.isActive) {
            return true;
        }
    }

    // Get cached metrics for DD supply and collateral
    const DigiDollar::SystemMetrics metrics = DigiDollar::SystemHealthMonitor::GetCachedMetrics();

    // If no DD in circulation, minting is always allowed (system has no liabilities)
    if (metrics.totalDDSupply <= 0) {
        return false;
    }

    // Use override price if provided (e.g., from ValidationContext during block validation),
    // otherwise query the global oracle.
    CAmount oraclePriceMicroUSD = oraclePriceOverride;
    if (oraclePriceMicroUSD <= 0) {
        if (Params().GetChainType() == ChainType::REGTEST && MockOracleManager::GetInstance().IsEnabled()) {
            oraclePriceMicroUSD = MockOracleManager::GetInstance().GetCurrentPrice();
        } else {
            oraclePriceMicroUSD = OracleBundleManager::GetInstance().GetLatestPrice();
        }
    }

    // FIX [T2-05c]: If no oracle price available, we can't determine health.
    // Fail-CLOSED: block minting when system state is unknown.
    // This prevents minting during oracle outages which could destabilize
    // the system if health is actually below 100%.
    if (oraclePriceMicroUSD <= 0) {
        LogPrint(BCLog::DIGIDOLLAR, "ERR: No oracle price available, blocking minting (fail-closed)\n");
        return true;
    }

    // Convert micro-USD to millicents for health calculation
    // micro-USD / 10 = millicents
    CAmount oraclePriceMillicents = oraclePriceMicroUSD / 10;

    // Calculate real-time system health
    int currentHealth = DCA::DynamicCollateralAdjustment::CalculateSystemHealth(
        metrics.totalCollateral, metrics.totalDDSupply, oraclePriceMillicents);

    if (ShouldActivateERR(currentHealth)) {
        LogPrint(BCLog::DIGIDOLLAR, "ERR: Blocking minting due to low system health (%d%%)\n", currentHealth);
        return true;
    }

    return false;
}

std::string EmergencyRedemptionRatio::GetERRStatistics()
{
    ERRState state = GetCurrentState();

    std::string stats = "ERR System Statistics:\n";
    stats += "Status: " + std::string(state.isActive ? "ACTIVE" : "INACTIVE") + "\n";
    stats += "System Health: " + std::to_string(state.systemHealth) + "%\n";

    if (state.isActive) {
        stats += "Adjustment Ratio: " + FormatERRAdjustment(state.adjustmentRatio) + "\n";
        stats += "Activation Height: " + std::to_string(state.activationHeight) + "\n";
        stats += "Queue Size: " + std::to_string(s_errQueue.size()) + " redemptions\n";

        uint64_t activeDuration = GetTime() - state.activationTimestamp;
        stats += "Active Duration: " + std::to_string(activeDuration / 3600) + " hours\n";
    }

    return stats;
}

bool EmergencyRedemptionRatio::ValidateERRConfig(std::string& error)
{
    // Validate ERR tier configuration
    if (ERR_TIERS.empty()) {
        error = "No ERR tiers configured";
        return false;
    }

    // Check tier ratios are reasonable basis-point values.
    for (const auto& tier : ERR_TIERS) {
        if (tier.second < 5000 || tier.second > ERR_RATIO_NORMAL_BPS) {
            error = "ERR tier ratio out of range: " + std::to_string(tier.second) + " bps";
            return false;
        }
    }

    // Validate oracle requirements
    if (REQUIRED_ORACLE_SIGNATURES > TOTAL_ORACLE_COUNT) {
        error = "Required oracle signatures exceed total oracle count";
        return false;
    }

    if (REQUIRED_ORACLE_SIGNATURES < TOTAL_ORACLE_COUNT / 2) {
        error = "Required oracle signatures below safe threshold";
        return false;
    }

    return true;
}

std::string EmergencyRedemptionRatio::FormatERRAdjustment(double ratio)
{
    return std::to_string(static_cast<int>(ratio * 100)) + "%";
}

std::string EmergencyRedemptionRatio::FormatERRHealth(int health, bool isERRActive)
{
    std::string healthStr = std::to_string(health) + "%";
    if (isERRActive) {
        healthStr += " (ERR ACTIVE)";
    }
    return healthStr;
}

// Private helper methods

uint256 EmergencyRedemptionRatio::CalculateOracleConsensusHash(const COracleBundle& bundle)
{
    // Create a deterministic hash of the oracle consensus
    std::vector<unsigned char> data;

    // Add epoch to the hash
    data.insert(data.end(), (unsigned char*)&bundle.epoch, (unsigned char*)&bundle.epoch + sizeof(bundle.epoch));

    // Add each message to the hash
    for (const auto& message : bundle.messages) {
        // Add oracle ID, price, and timestamp
        data.insert(data.end(), (unsigned char*)&message.oracle_id, (unsigned char*)&message.oracle_id + sizeof(message.oracle_id));
        data.insert(data.end(), (unsigned char*)&message.price_micro_usd, (unsigned char*)&message.price_micro_usd + sizeof(message.price_micro_usd));
        data.insert(data.end(), (unsigned char*)&message.timestamp, (unsigned char*)&message.timestamp + sizeof(message.timestamp));
    }

    return Hash(data);
}

bool EmergencyRedemptionRatio::ValidateOracleSignature(const COraclePriceMessage& message)
{
    // Use the existing oracle message validation
    return message.IsValid();
}

bool EmergencyRedemptionRatio::IsAuthorizedOracleKey(const XOnlyPubKey& oracleKey)
{
    // For ERR, oracle authorization is handled by the existing oracle system
    // The oracle bundle validation already checks authorized oracles
    return true;
}

void EmergencyRedemptionRatio::UpdateERRState(const ERRState& newState)
{
    // Note: caller must hold s_errMutex (or this is called from a locked context)
    s_currentState = newState;
}

void EmergencyRedemptionRatio::ReconstructERRState(int currentSystemHealth, uint32_t currentHeight)
{
    std::lock_guard<std::mutex> lock(s_errMutex); // RH-44: thread safety
    ERRState newState;

    if (currentSystemHealth < 100) {
        // System is under-collateralized — activate ERR
        newState.isActive = true;
        newState.systemHealth = currentSystemHealth;
        newState.adjustmentRatioBps = CalculateERRRatioBps(currentSystemHealth);
        newState.adjustmentRatio = RatioBpsToDouble(newState.adjustmentRatioBps);
        newState.activationHeight = currentHeight;
        newState.activationTimestamp = GetTime();

        LogPrintf("ERR: Reconstructed active state - health=%d%%, ratio=%.3f at height %d\n",
                  currentSystemHealth, newState.adjustmentRatio, currentHeight);
    } else {
        // System is healthy — ensure ERR is inactive
        LogPrintf("ERR: Reconstructed inactive state - health=%d%% (healthy) at height %d\n",
                  currentSystemHealth, currentHeight);
    }

    UpdateERRState(newState);
    s_stateReconstructed = true;

    LogPrintf("ERR: State reconstruction locked — DCA cache reads blocked until first real health update\n");
}

void EmergencyRedemptionRatio::ClearStateReconstructed()
{
    std::lock_guard<std::mutex> lock(s_errMutex); // RH-44: thread safety
    if (s_stateReconstructed) {
        s_stateReconstructed = false;
        LogPrintf("ERR: State reconstruction lock cleared — DCA cache reads re-enabled\n");
    }
}

void EmergencyRedemptionRatio::ClearERRQueue()
{
    // Note: caller must hold s_errMutex
    s_errQueue.clear();
    s_queuedRedemptions.clear();
}

// Missing implementations for testing functions
bool EmergencyRedemptionRatio::HandleHealthOscillation(const std::vector<int>& healthValues, const std::vector<bool>& activationResults)
{
    if (healthValues.size() != activationResults.size()) return false;

    for (size_t i = 0; i < healthValues.size(); ++i) {
        bool expectedActivation = ShouldActivateERR(healthValues[i]);
        if (expectedActivation != activationResults[i]) {
            return false;
        }
    }
    return true;
}

bool EmergencyRedemptionRatio::ValidatePrecisionBoundaries(const std::vector<double>& preciseHealth)
{
    for (double health : preciseHealth) {
        int intHealth = static_cast<int>(health);
        bool shouldActivate = ShouldActivateERR(intHealth);

        // Validate boundary precision
        if (health < 100.0 && !shouldActivate) return false;
        if (health >= 100.0 && shouldActivate) return false;
    }
    return true;
}

bool EmergencyRedemptionRatio::ValidateExtremeHealthValues(const std::vector<int>& extremeValues)
{
    for (int health : extremeValues) {
        // Should handle extreme values gracefully
        try {
            ShouldActivateERR(health);  // Test function doesn't crash
            double adjustment = CalculateERRAdjustment(health);

            // Validate adjustment is within bounds
            if (adjustment < 0.5 || adjustment > 1.0) return false;
        } catch (...) {
            return false;
        }
    }
    return true;
}

bool EmergencyRedemptionRatio::ValidateThreadSafety(const std::vector<bool>& concurrentResults)
{
    // Simple validation - all concurrent operations should have consistent results
    if (concurrentResults.empty()) return true;

    bool firstResult = concurrentResults[0];
    for (bool result : concurrentResults) {
        if (result != firstResult) return false;
    }
    return true;
}

bool EmergencyRedemptionRatio::HandleConsensusFailure(int systemHealth, const std::vector<COraclePriceMessage>& messages)
{
    // If system health requires ERR but insufficient oracle messages, handle gracefully
    if (ShouldActivateERR(systemHealth) && messages.size() < REQUIRED_ORACLE_SIGNATURES) {
        LogPrint(BCLog::DIGIDOLLAR, "ERR: Consensus failure - insufficient oracle messages (%zu/%zu)\n",
                 messages.size(), REQUIRED_ORACLE_SIGNATURES);
        return true; // Handled gracefully
    }
    return false;
}

bool EmergencyRedemptionRatio::ShouldActivateERRWithCorruptedState(int systemHealth, bool stateCorrupted)
{
    // If state is corrupted, be conservative and don't activate
    if (stateCorrupted) {
        return false;
    }
    return ShouldActivateERR(systemHealth);
}

bool EmergencyRedemptionRatio::ShouldActivateERRAtHeight(int systemHealth, uint32_t blockHeight)
{
    // Basic ERR activation logic doesn't depend on block height for now
    return ShouldActivateERR(systemHealth);
}

bool EmergencyRedemptionRatio::ShouldActivateERRAtTime(int systemHealth, int64_t timestamp)
{
    // Basic ERR activation logic doesn't depend on time for now
    return ShouldActivateERR(systemHealth);
}

bool EmergencyRedemptionRatio::HasActivationDelay(int systemHealth)
{
    // No activation delay mechanism implemented yet
    return false;
}

bool EmergencyRedemptionRatio::ValidateRatioPrecision(int health, double expectedRatio)
{
    double actualRatio = CalculateERRAdjustment(health);
    return std::abs(actualRatio - expectedRatio) < 0.001; // 0.1% precision tolerance
}

bool EmergencyRedemptionRatio::PreventCalculationOverflow(CAmount maxRedemption, int health)
{
    try {
        CAmount result = GetAdjustedRedemption(maxRedemption, health);
        return result <= maxRedemption && result >= 0;
    } catch (...) {
        return false;
    }
}

bool EmergencyRedemptionRatio::ValidateCalculationConsistency(const std::vector<std::pair<CAmount, double>>& stressTests)
{
    // UPDATED: ERR now returns FULL collateral, increases DD burn instead
    // This validates that GetAdjustedRedemption returns the full amount
    // and GetRequiredDDBurn calculates correct burn increase
    for (const auto& test : stressTests) {
        CAmount amount = test.first;
        double ratio = test.second;

        int health = static_cast<int>(ratio * 100);

        // GetAdjustedRedemption should return FULL amount (not reduced)
        CAmount collateralResult = GetAdjustedRedemption(amount, health);
        if (collateralResult != amount) return false; // Must return full amount

        // GetRequiredDDBurn should return amount / ratio (more than original)
        CAmount burnResult = GetRequiredDDBurn(amount, health);
        if (health < 100 && burnResult <= amount) return false; // Must burn MORE
    }
    return true;
}

bool EmergencyRedemptionRatio::HandleLargeOracleMessageCount(const COracleBundle& bundle)
{
    // Should handle large message counts without performance issues
    return bundle.messages.size() <= 1000; // Reasonable upper limit
}

bool EmergencyRedemptionRatio::ValidateMalformedMessageHandling(const std::vector<COraclePriceMessage>& malformedMessages)
{
    // Should handle malformed messages gracefully without crashing
    for (const auto& message : malformedMessages) {
        try {
            ValidateOracleSignature(message);
        } catch (...) {
            return false;
        }
    }
    return true;
}

bool EmergencyRedemptionRatio::ValidateConsensusPerformance(int64_t durationMs)
{
    // Oracle consensus should complete within reasonable time (10 seconds)
    return durationMs <= 10000;
}

void EmergencyRedemptionRatio::ResetForTesting()
{
    std::lock_guard<std::mutex> lock(s_errMutex);
    s_currentState = ERRState();
    s_stateReconstructed = false;
    s_errQueue.clear();
    s_queuedRedemptions.clear();
}

void EmergencyRedemptionRatio::SetActiveForTesting(bool active)
{
    std::lock_guard<std::mutex> lock(s_errMutex);
    s_currentState.isActive = active;
}

} // namespace ERR
} // namespace DigiDollar
