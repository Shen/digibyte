// Copyright (c) 2024 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_ORACLE_EXCHANGE_H
#define DIGIBYTE_ORACLE_EXCHANGE_H

#include <consensus/amount.h>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/**
 * Exchange-specific price fetchers
 * Real implementations for fetching DGB/USD prices from major exchanges
 */

namespace ExchangeAPI {

/**
 * Base Exchange Price Fetcher
 */
class BaseExchangeFetcher
{
protected:
    std::string exchange_name;
    std::string base_url;
    int timeout_seconds{10};

    //! Persistent CURL handle — reused across requests to avoid socket exhaustion.
    //! On Windows, curl_easy_init()/cleanup() per request causes TIME_WAIT buildup
    //! that exhausts ephemeral ports after 6-24 hours with 7 exchanges @ 15s intervals.
    void* m_curl_handle{nullptr};
    std::function<bool()> m_interrupt_callback;

public:
    BaseExchangeFetcher(const std::string& name, const std::string& url);

    virtual ~BaseExchangeFetcher();

    //! Fetch current DGB/USD price in micro-USD (e.g., 50000 = $0.05)
    virtual CAmount FetchPrice() = 0;

    //! Get exchange name
    const std::string& GetExchangeName() const { return exchange_name; }

    //! Set request timeout
    void SetTimeout(int seconds) { timeout_seconds = seconds; }
    void SetInterruptCallback(std::function<bool()> callback) { m_interrupt_callback = std::move(callback); }

    //! Convert price string to micro-USD (1,000,000 = $1.00) - Public for testing
    CAmount ConvertToMicroUSD(const std::string& price_str);
    CAmount ConvertToMicroUSD(double price_usd);

    //! Parse JSON response - Public for testing
    std::string ExtractJsonValue(const std::string& json, const std::string& key);

protected:
    //! Make HTTP GET request
    virtual std::string HttpGet(const std::string& url);
};

/**
 * Binance Exchange Fetcher
 * Fetches from Binance DGBUSDT or DGBBTC->BTCUSDT pair
 */
class BinanceFetcher : public BaseExchangeFetcher
{
public:
    BinanceFetcher();
    CAmount FetchPrice() override;

private:
    CAmount FetchDGBUSDT();
    CAmount FetchDGBBTC_BTCUSDT();
};

/**
 * Coinbase Exchange Fetcher
 * Fetches from Coinbase Pro or regular Coinbase API
 */
class CoinbaseFetcher : public BaseExchangeFetcher
{
public:
    CoinbaseFetcher();
    CAmount FetchPrice() override;

private:
    CAmount FetchFromCoinbasePro();
    CAmount FetchFromCoinbaseRegular();
};

/**
 * Kraken Exchange Fetcher
 * Fetches DGB/USD from Kraken
 */
class KrakenFetcher : public BaseExchangeFetcher
{
public:
    KrakenFetcher();
    CAmount FetchPrice() override;

private:
    std::string ParseKrakenResponse(const std::string& response);
};

// CoinMarketCap removed — requires paid API key, incompatible with decentralized oracle design

/**
 * CoinGecko Fetcher
 * Fetches DGB/USD from CoinGecko public API
 * No API key required
 */
class CoinGeckoFetcher : public BaseExchangeFetcher
{
public:
    CoinGeckoFetcher();
    CAmount FetchPrice() override;
};

/**
 * Bittrex Exchange Fetcher
 * Fetches DGB/USD from Bittrex
 */
class BittrexFetcher : public BaseExchangeFetcher
{
public:
    BittrexFetcher();
    CAmount FetchPrice() override;

private:
    std::string ParseBittrexResponse(const std::string& response);
};

/**
 * Poloniex Exchange Fetcher
 * Fetches DGB/USDT from Poloniex
 */
class PoloniexFetcher : public BaseExchangeFetcher
{
public:
    PoloniexFetcher();
    CAmount FetchPrice() override;

private:
    std::string ParsePoloniexResponse(const std::string& response);
};

/**
 * Messari Fetcher
 * Fetches DGB/USD from Messari API
 * No API key required
 */
class MessariFetcher : public BaseExchangeFetcher
{
public:
    MessariFetcher();
    CAmount FetchPrice() override;
};

/**
 * KuCoin Exchange Fetcher
 * Fetches DGB/USDT from KuCoin
 * No API key required for public endpoints
 */
class KuCoinFetcher : public BaseExchangeFetcher
{
public:
    KuCoinFetcher();
    CAmount FetchPrice() override;
};

/**
 * Crypto.com Exchange Fetcher
 * Fetches DGB/USD from Crypto.com
 * No API key required for public endpoints
 */
class CryptoComFetcher : public BaseExchangeFetcher
{
public:
    CryptoComFetcher();
    CAmount FetchPrice() override;
};

/**
 * Gate.io Exchange Fetcher
 * Fetches DGB/USDT from Gate.io
 * No API key required for public endpoints
 */
class GateIOFetcher : public BaseExchangeFetcher
{
public:
    GateIOFetcher();
    CAmount FetchPrice() override;
};

/**
 * HTX (Huobi) Exchange Fetcher
 * Fetches DGB/USDT from HTX
 * No API key required for public endpoints
 */
class HTXFetcher : public BaseExchangeFetcher
{
public:
    HTXFetcher();
    CAmount FetchPrice() override;
};

/**
 * Multi-Exchange Price Aggregator
 * Fetches from multiple exchanges and calculates median/weighted average
 */
class MultiExchangeAggregator
{
public:
    struct ExchangePrice {
        std::string exchange;
        CAmount price_micro_usd;  // Price in micro-USD (1,000,000 = $1.00)
        int64_t timestamp;
        bool success;
        double weight;

        ExchangePrice() : price_micro_usd(0), timestamp(0), success(false), weight(1.0) {}
        ExchangePrice(const std::string& exchange_name, CAmount price, int64_t time, bool ok, double w = 1.0)
            : exchange(exchange_name), price_micro_usd(price), timestamp(time), success(ok), weight(w) {}
    };

private:
    std::vector<std::unique_ptr<BaseExchangeFetcher>> fetchers;
    std::vector<ExchangePrice> last_prices;

    // Configuration
    size_t min_required_sources{2};  // Lowered from 3 - only need 2 working exchanges
    double outlier_threshold{0.10}; // 10% deviation
    bool use_weighted_median{false};
    std::function<bool()> interrupt_callback;

public:
    MultiExchangeAggregator();
    explicit MultiExchangeAggregator(std::vector<std::unique_ptr<BaseExchangeFetcher>> custom_fetchers);
    ~MultiExchangeAggregator();

    //! Configuration
    void SetMinRequiredSources(size_t min_sources) { min_required_sources = min_sources; }
    void SetOutlierThreshold(double threshold) { outlier_threshold = threshold; }
    void SetUseWeightedMedian(bool use_weighted) { use_weighted_median = use_weighted; }
    void SetInterruptCallback(std::function<bool()> callback);

    //! Price fetching
    CAmount FetchAggregatePrice();
    std::vector<ExchangePrice> FetchAllPrices();

    //! Price calculation methods
    CAmount CalculateMedianPrice(const std::vector<ExchangePrice>& prices);
    CAmount CalculateWeightedMedian(const std::vector<ExchangePrice>& prices);
    CAmount CalculateWeightedAverage(const std::vector<ExchangePrice>& prices);

    //! Outlier detection and filtering
    std::vector<ExchangePrice> FilterOutliers(const std::vector<ExchangePrice>& prices);
    bool IsOutlier(CAmount price, const std::vector<ExchangePrice>& prices);

    //! Status and validation
    bool HasSufficientData() const;
    size_t GetSuccessfulSourceCount() const;
    std::vector<ExchangePrice> GetLastPrices() const { return last_prices; }

    //! Exchange weights (for weighted calculations)
    void SetExchangeWeight(const std::string& exchange, double weight);
    double GetExchangeWeight(const std::string& exchange);

private:
    //! Internal helpers
    void InitializeFetchers();
    std::vector<ExchangePrice> FilterValidPrices(const std::vector<ExchangePrice>& prices);
    void LogPriceResults(const std::vector<ExchangePrice>& prices, CAmount final_price);
};

} // namespace ExchangeAPI

#endif // DIGIBYTE_ORACLE_EXCHANGE_H
