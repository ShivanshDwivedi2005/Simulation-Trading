#pragma once

#include "config/Config.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace simtrade::market {

class MarketDataRestClient {
public:
  explicit MarketDataRestClient(const config::Config &config);

  [[nodiscard]] nlohmann::json quote(const std::string &symbol);
  [[nodiscard]] std::vector<nlohmann::json>
  latest_snapshots(const std::vector<std::string> &symbols);
  [[nodiscard]] nlohmann::json historical_bars(const std::string &symbol,
                                               std::size_t limit = 200);

  [[nodiscard]] std::size_t request_count() const noexcept;
  [[nodiscard]] std::size_t rate_limit_wait_count() const noexcept;

private:
  [[nodiscard]] nlohmann::json authenticated_get(const std::string &url);
  void wait_for_rate_limit_slot();

  const config::Config &config_;
  mutable std::mutex rate_limit_mutex_;
  std::chrono::steady_clock::time_point next_request_at_{};
  std::atomic<std::size_t> request_count_{0};
  std::atomic<std::size_t> rate_limit_wait_count_{0};
};

[[nodiscard]] bool
market_data_timestamp_is_fresh(const std::string &timestamp,
                               std::chrono::seconds maximum_age,
                               std::chrono::system_clock::time_point now =
                                   std::chrono::system_clock::now());

} // namespace simtrade::market
