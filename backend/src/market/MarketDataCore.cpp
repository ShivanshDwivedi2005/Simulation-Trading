#include "market/MarketDataCore.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace {

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

std::string now_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
         << milliseconds << 'Z';
  return output.str();
}

std::set<std::string> acknowledgement_symbols(const nlohmann::json& acknowledgement) {
  std::set<std::string> symbols;
  for (const auto* field : {"trades", "quotes", "bars", "updatedBars"}) {
    if (!acknowledgement.contains(field) || !acknowledgement[field].is_array()) continue;
    for (const auto& value : acknowledgement[field]) {
      if (value.is_string()) symbols.insert(simtrade::market::normalize_symbol(value.get<std::string>()));
    }
  }
  return symbols;
}

}  // namespace

namespace simtrade::market {

SubscriptionRegistry::SubscriptionRegistry(std::size_t maximum_symbols)
    : maximum_symbols_(maximum_symbols) {}

SubscriptionResult SubscriptionRegistry::request(const std::string& raw_symbol) {
  const auto symbol = normalize_symbol(raw_symbol);
  if (!valid_symbol(symbol)) return SubscriptionResult::invalid_symbol;
  std::scoped_lock lock(mutex_);
  if (desired_.contains(symbol)) return SubscriptionResult::duplicate;
  if (desired_.size() >= maximum_symbols_) return SubscriptionResult::limit_reached;
  desired_.insert(symbol);
  return SubscriptionResult::added;
}

bool SubscriptionRegistry::release(const std::string& raw_symbol) {
  const auto symbol = normalize_symbol(raw_symbol);
  std::scoped_lock lock(mutex_);
  return desired_.erase(symbol) > 0;
}

void SubscriptionRegistry::confirm(const nlohmann::json& acknowledgement) {
  const auto symbols = acknowledgement_symbols(acknowledgement);
  std::scoped_lock lock(mutex_);
  confirmed_ = symbols;
}

void SubscriptionRegistry::disconnected() {
  std::scoped_lock lock(mutex_);
  confirmed_.clear();
}

std::set<std::string> SubscriptionRegistry::desired() const {
  std::scoped_lock lock(mutex_);
  return desired_;
}

std::set<std::string> SubscriptionRegistry::confirmed() const {
  std::scoped_lock lock(mutex_);
  return confirmed_;
}

std::size_t SubscriptionRegistry::maximum() const noexcept { return maximum_symbols_; }

std::string normalize_symbol(std::string symbol) {
  symbol.erase(std::remove_if(symbol.begin(), symbol.end(), [](unsigned char character) {
    return std::isspace(character);
  }), symbol.end());
  std::transform(symbol.begin(), symbol.end(), symbol.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return symbol;
}

bool valid_symbol(const std::string& symbol) {
  return !symbol.empty() && symbol.size() <= 12 &&
         std::all_of(symbol.begin(), symbol.end(), [](unsigned char character) {
           return std::isalnum(character) || character == '.' || character == '-';
         });
}

std::vector<Instrument> parse_alpaca_assets(const nlohmann::json& assets) {
  std::vector<Instrument> instruments;
  if (!assets.is_array()) return instruments;
  for (const auto& asset : assets) {
    if (!asset.is_object()) continue;
    const auto asset_class = asset.value("class", "");
    const auto status = asset.value("status", "");
    const auto symbol = normalize_symbol(asset.value("symbol", ""));
    if (asset_class != "us_equity" || status != "active" || !valid_symbol(symbol)) continue;
    instruments.push_back({
        .symbol = symbol,
        .name = asset.value("name", symbol),
        .exchange = asset.value("exchange", ""),
        .asset_class = asset_class,
        .active = true,
        .tradable = asset.value("tradable", false),
        .fractionable = asset.value("fractionable", false),
    });
  }
  std::sort(instruments.begin(), instruments.end(), [](const Instrument& left, const Instrument& right) {
    return left.symbol < right.symbol;
  });
  instruments.erase(std::unique(instruments.begin(), instruments.end(), [](const Instrument& left, const Instrument& right) {
    return left.symbol == right.symbol;
  }), instruments.end());
  return instruments;
}

InstrumentSearchPage search_instruments(const std::vector<Instrument>& catalogue,
                                        const std::string& query,
                                        std::size_t page,
                                        std::size_t limit) {
  page = std::max<std::size_t>(1, page);
  limit = std::clamp<std::size_t>(limit, 1, 100);
  const auto needle = lowercase(query);
  std::vector<const Instrument*> matches;
  matches.reserve(catalogue.size());
  for (const auto& instrument : catalogue) {
    if (needle.empty() || lowercase(instrument.symbol).find(needle) != std::string::npos ||
        lowercase(instrument.name).find(needle) != std::string::npos) {
      matches.push_back(&instrument);
    }
  }
  const auto offset = (page - 1) * limit;
  InstrumentSearchPage result{.instruments = {}, .page = page, .limit = limit, .total = matches.size()};
  if (offset >= matches.size()) return result;
  const auto end = std::min(matches.size(), offset + limit);
  result.instruments.reserve(end - offset);
  for (auto index = offset; index < end; ++index) result.instruments.push_back(*matches[index]);
  return result;
}

nlohmann::json instrument_to_json(const Instrument& instrument) {
  return {
      {"symbol", instrument.symbol},
      {"name", instrument.name},
      {"exchange", instrument.exchange},
      {"assetClass", instrument.asset_class},
      {"active", instrument.active},
      {"tradable", instrument.tradable},
      {"fractionable", instrument.fractionable},
  };
}

nlohmann::json authentication_message(const std::string& key, const std::string& secret) {
  return {{"action", "auth"}, {"key", key}, {"secret", secret}};
}

nlohmann::json subscription_message(const std::string& action, const std::set<std::string>& symbols) {
  const std::vector<std::string> values(symbols.begin(), symbols.end());
  return {{"action", action}, {"trades", values}, {"quotes", values}, {"bars", values}, {"updatedBars", values}};
}

std::vector<NormalizedEvent> normalize_alpaca_events(const nlohmann::json& message,
                                                    const std::string& feed) {
  std::vector<NormalizedEvent> events;
  const auto values = message.is_array() ? message : nlohmann::json::array({message});
  for (const auto& value : values) {
    if (!value.is_object()) continue;
    const auto alpaca_type = value.value("T", "");
    const auto symbol = normalize_symbol(value.value("S", ""));
    if (!valid_symbol(symbol)) continue;

    nlohmann::json payload{
        {"symbol", symbol},
        {"timestamp", value.value("t", "")},
        {"cachedAt", now_iso8601()},
        {"source", "alpaca_" + feed},
        {"live", true},
    };
    std::string type;
    std::string key;
    if (alpaca_type == "q") {
      type = "quote";
      key = "market:quote:" + symbol;
      payload.update({{"bidPrice", value.value("bp", 0.0)}, {"bidSize", value.value("bs", 0.0)},
                      {"askPrice", value.value("ap", 0.0)}, {"askSize", value.value("as", 0.0)}});
    } else if (alpaca_type == "t") {
      type = "trade";
      key = "market:trade:" + symbol;
      payload.update({{"price", value.value("p", 0.0)}, {"size", value.value("s", 0.0)}});
    } else if (alpaca_type == "b" || alpaca_type == "u") {
      type = alpaca_type == "b" ? "bar" : "updatedBar";
      key = "market:bar:1m:" + symbol;
      payload.update({{"timeframe", "1m"}, {"open", value.value("o", 0.0)},
                      {"high", value.value("h", 0.0)}, {"low", value.value("l", 0.0)},
                      {"close", value.value("c", 0.0)}, {"volume", value.value("v", 0.0)}});
    } else {
      continue;
    }
    payload["type"] = type;
    events.push_back({type, symbol, key, std::move(payload)});
  }
  return events;
}

void cache_normalized_event(const NormalizedEvent& event, const CacheWriter& writer) {
  if (writer && !event.cache_key.empty()) writer(event.cache_key, event.payload.dump());
}

}  // namespace simtrade::market
