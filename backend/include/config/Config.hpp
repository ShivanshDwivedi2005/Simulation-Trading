#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace simtrade::config {

struct Config {
  std::string app_env;
  std::string http_host;
  std::uint16_t http_port;
  std::size_t worker_threads;
  std::string database_url;
  std::string redis_url;
  std::string cors_allowed_origins;
  std::string log_level;
  std::string alpaca_api_key_id;
  std::string alpaca_api_secret_key;
  std::string alpaca_trading_rest_url;
  std::string alpaca_data_rest_url;
  std::string alpaca_data_ws_url;
  std::string alpaca_data_feed;
  std::uint32_t alpaca_max_stream_symbols;
  std::uint32_t alpaca_asset_sync_interval_hours;
  std::vector<std::string> alpaca_pinned_symbols;
  std::uint32_t market_data_eviction_grace_seconds;
  std::uint32_t market_data_min_residency_seconds;
  std::string smtp_host;
  std::uint16_t smtp_port;
  std::string smtp_username;
  std::string smtp_password;
  std::string smtp_from;
  bool smtp_use_tls;
  std::uint32_t otp_ttl_seconds;
  std::string otp_pepper;
  std::uint32_t access_token_ttl_seconds;

  static Config from_environment();
};

}  // namespace simtrade::config
