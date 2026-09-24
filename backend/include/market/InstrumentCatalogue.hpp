#pragma once

#include "cache/RedisClient.hpp"
#include "config/Config.hpp"
#include "market/MarketDataCore.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace simtrade::market {

class InstrumentCatalogue {
 public:
  InstrumentCatalogue(const config::Config& config, const cache::RedisClient& redis);
  ~InstrumentCatalogue();
  InstrumentCatalogue(const InstrumentCatalogue&) = delete;
  InstrumentCatalogue& operator=(const InstrumentCatalogue&) = delete;

  void start();
  void stop();
  [[nodiscard]] InstrumentSearchPage search(const std::string& query,
                                            std::size_t page,
                                            std::size_t limit) const;
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] bool contains(const std::string& symbol) const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::string last_synced_at() const;
  [[nodiscard]] std::string last_error() const;

 private:
  bool load_cached_catalogue();
  void synchronize();
  void run();

  const config::Config& config_;
  const cache::RedisClient& redis_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<Instrument> instruments_;
  std::string last_synced_at_;
  std::string last_error_;
  std::atomic<bool> running_{false};
  std::thread worker_;
};

}  // namespace simtrade::market
