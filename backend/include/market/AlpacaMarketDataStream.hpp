#pragma once

#include "cache/RedisClient.hpp"
#include "config/Config.hpp"
#include "market/MarketDataProvider.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace simtrade::market {

class AlpacaMarketDataStream final : public MarketDataProvider {
 public:
  AlpacaMarketDataStream(const config::Config& config, const cache::RedisClient& redis);
  ~AlpacaMarketDataStream() override;
  AlpacaMarketDataStream(const AlpacaMarketDataStream&) = delete;
  AlpacaMarketDataStream& operator=(const AlpacaMarketDataStream&) = delete;

  void start(EventHandler handler) override;
  void stop() override;
  SubscriptionResult subscribe(const std::string& symbol) override;
  bool unsubscribe(const std::string& symbol) override;
  [[nodiscard]] MarketDataHealth health() const override;
  [[nodiscard]] const SubscriptionRegistry& subscriptions() const noexcept;

 private:
  void run();
  void connect_and_stream();
  void handle_message(const std::string& message);
  void publish_status(bool connected);

  const config::Config& config_;
  const cache::RedisClient& redis_;
  SubscriptionRegistry subscriptions_;
  std::atomic<bool> running_{false};
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  EventHandler handler_;
  bool connected_{false};
  std::string last_message_at_;
  std::string last_error_;
  std::uint64_t reconnect_count_{0};
};

}  // namespace simtrade::market
