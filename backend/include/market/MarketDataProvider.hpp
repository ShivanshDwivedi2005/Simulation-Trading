#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace simtrade::market {

struct Quote {
  std::string instrument_id;
  std::string symbol;
  double bid;
  double ask;
  double last;
  std::chrono::system_clock::time_point received_at;
};

class MarketDataProvider {
 public:
  using QuoteHandler = std::function<void(const Quote&)>;
  virtual ~MarketDataProvider() = default;
  virtual void start(QuoteHandler handler) = 0;
  virtual void subscribe(const std::vector<std::string>& symbols) = 0;
  virtual void unsubscribe(const std::vector<std::string>& symbols) = 0;
  [[nodiscard]] virtual std::optional<Quote> latest_quote(const std::string& symbol) const = 0;
};

}  // namespace simtrade::market
