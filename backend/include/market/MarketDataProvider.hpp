#pragma once

#include "market/MarketDataCore.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace simtrade::market {

struct Quote {
  std::string instrument_id;
  std::string symbol;
  double bid{0.0};
  double ask{0.0};
  double last{0.0};
  std::chrono::system_clock::time_point received_at;
};

struct MarketDataHealth {
  bool connected{false};
  std::string feed;
  std::size_t subscribed_symbol_count{0};
  std::size_t maximum_symbol_count{0};
  std::string last_message_at;
  std::string last_error;
  std::uint64_t reconnect_count{0};
};

class MarketDataProvider : public SymbolSubscriptionSink {
 public:
  using EventHandler = std::function<void(const nlohmann::json&)>;
  virtual ~MarketDataProvider() = default;
  virtual void start(EventHandler handler) = 0;
  virtual void stop() = 0;
  [[nodiscard]] virtual MarketDataHealth health() const = 0;
};

}  // namespace simtrade::market
