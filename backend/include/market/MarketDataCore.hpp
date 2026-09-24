#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace simtrade::market {

struct Instrument {
  std::string symbol;
  std::string name;
  std::string exchange;
  std::string asset_class;
  bool active{false};
  bool tradable{false};
  bool fractionable{false};
};

struct InstrumentSearchPage {
  std::vector<Instrument> instruments;
  std::size_t page{1};
  std::size_t limit{20};
  std::size_t total{0};
};

enum class SubscriptionResult { added, duplicate, limit_reached, invalid_symbol };

class SymbolSubscriptionSink {
 public:
  virtual ~SymbolSubscriptionSink() = default;
  virtual SubscriptionResult subscribe(const std::string& symbol) = 0;
  virtual bool unsubscribe(const std::string& symbol) = 0;
};

class SubscriptionRegistry {
 public:
  explicit SubscriptionRegistry(std::size_t maximum_symbols);
  SubscriptionResult request(const std::string& symbol);
  bool release(const std::string& symbol);
  void confirm(const nlohmann::json& acknowledgement);
  void disconnected();
  [[nodiscard]] std::set<std::string> desired() const;
  [[nodiscard]] std::set<std::string> confirmed() const;
  [[nodiscard]] std::size_t maximum() const noexcept;

 private:
  std::size_t maximum_symbols_;
  mutable std::mutex mutex_;
  std::set<std::string> desired_;
  std::set<std::string> confirmed_;
};

struct NormalizedEvent {
  std::string type;
  std::string symbol;
  std::string cache_key;
  nlohmann::json payload;
};

using CacheWriter = std::function<void(const std::string&, const std::string&)>;

[[nodiscard]] std::string normalize_symbol(std::string symbol);
[[nodiscard]] bool valid_symbol(const std::string& symbol);
[[nodiscard]] std::vector<Instrument> parse_alpaca_assets(const nlohmann::json& assets);
[[nodiscard]] InstrumentSearchPage search_instruments(const std::vector<Instrument>& catalogue,
                                                      const std::string& query,
                                                      std::size_t page,
                                                      std::size_t limit);
[[nodiscard]] nlohmann::json instrument_to_json(const Instrument& instrument);
[[nodiscard]] nlohmann::json authentication_message(const std::string& key, const std::string& secret);
[[nodiscard]] nlohmann::json subscription_message(const std::string& action,
                                                  const std::set<std::string>& symbols);
[[nodiscard]] std::vector<NormalizedEvent> normalize_alpaca_events(const nlohmann::json& message,
                                                                  const std::string& feed);
void cache_normalized_event(const NormalizedEvent& event, const CacheWriter& writer);

}  // namespace simtrade::market
