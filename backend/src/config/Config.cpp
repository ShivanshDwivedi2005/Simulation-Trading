#include "config/Config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

std::string env_or(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  return value == nullptr || *value == '\0' ? std::string(fallback) : std::string(value);
}

std::uint16_t env_port(const char* name, std::uint16_t fallback) {
  const auto value = env_or(name, "");
  if (value.empty()) return fallback;
  const auto parsed = std::stoul(value);
  if (parsed == 0 || parsed > 65535) throw std::runtime_error(std::string(name) + " must be between 1 and 65535");
  return static_cast<std::uint16_t>(parsed);
}

std::uint32_t env_positive_integer(const char* name, std::uint32_t fallback) {
  const auto value = env_or(name, "");
  if (value.empty()) return fallback;
  const auto parsed = std::stoul(value);
  if (parsed == 0) throw std::runtime_error(std::string(name) + " must be greater than zero");
  return static_cast<std::uint32_t>(parsed);
}

bool env_boolean(const char* name, bool fallback) {
  auto value = env_or(name, fallback ? "true" : "false");
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  if (value == "true" || value == "1" || value == "yes") return true;
  if (value == "false" || value == "0" || value == "no") return false;
  throw std::runtime_error(std::string(name) + " must be true or false");
}

std::vector<std::string> pinned_symbols() {
  const auto configured = env_or("ALPACA_PINNED_SYMBOLS", "AAPL,MSFT,NVDA,AMZN,GOOGL");
  std::vector<std::string> symbols;
  std::set<std::string> unique;
  std::size_t start = 0;
  while (start <= configured.size()) {
    const auto comma = configured.find(',', start);
    auto symbol = configured.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    symbol.erase(std::remove_if(symbol.begin(), symbol.end(), [](unsigned char character) {
      return std::isspace(character);
    }), symbol.end());
    std::transform(symbol.begin(), symbol.end(), symbol.begin(), [](unsigned char character) {
      return static_cast<char>(std::toupper(character));
    });
    if (symbol.empty()) throw std::runtime_error("ALPACA_PINNED_SYMBOLS contains an empty symbol");
    if (!unique.insert(symbol).second) {
      throw std::runtime_error("ALPACA_PINNED_SYMBOLS contains duplicate symbol: " + symbol);
    }
    symbols.push_back(std::move(symbol));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  if (symbols.size() != 5) {
    throw std::runtime_error("ALPACA_PINNED_SYMBOLS must contain exactly 5 unique symbols; found " +
                             std::to_string(symbols.size()));
  }
  return symbols;
}

}  // namespace

namespace simtrade::config {

Config Config::from_environment() {
  const auto hardware_threads = std::max(2u, std::thread::hardware_concurrency());
  auto config = Config{
      .app_env = env_or("APP_ENV", "development"),
      .http_host = env_or("HTTP_HOST", "0.0.0.0"),
      .http_port = env_port("HTTP_PORT", 8080),
      .worker_threads = hardware_threads,
      .database_url = env_or("DATABASE_URL", "postgresql://simtrade:simtrade-local-only@localhost:5432/simulation_trading"),
      .redis_url = env_or("REDIS_URL", "redis://localhost:6379/0"),
      .cors_allowed_origins = env_or("CORS_ALLOWED_ORIGINS", "http://localhost:3000"),
      .log_level = env_or("LOG_LEVEL", "info"),
      .alpaca_api_key_id = env_or("ALPACA_API_KEY_ID", ""),
      .alpaca_api_secret_key = env_or("ALPACA_API_SECRET_KEY", ""),
      .alpaca_trading_rest_url = env_or("ALPACA_TRADING_REST_URL", "https://paper-api.alpaca.markets"),
      .alpaca_data_rest_url = env_or("ALPACA_DATA_REST_URL", "https://data.alpaca.markets"),
      .alpaca_data_ws_url = env_or("ALPACA_DATA_WS_URL", "wss://stream.data.alpaca.markets/v2/iex"),
      .alpaca_data_feed = env_or("ALPACA_DATA_FEED", "iex"),
      .alpaca_max_stream_symbols = env_positive_integer("ALPACA_MAX_STREAM_SYMBOLS", 30),
      .alpaca_asset_sync_interval_hours = env_positive_integer("ALPACA_ASSET_SYNC_INTERVAL_HOURS", 24),
      .alpaca_pinned_symbols = pinned_symbols(),
      .market_data_eviction_grace_seconds = env_positive_integer("MARKET_DATA_EVICTION_GRACE_SECONDS", 30),
      .market_data_min_residency_seconds = env_positive_integer("MARKET_DATA_MIN_RESIDENCY_SECONDS", 30),
      .smtp_host = env_or("SMTP_HOST", ""),
      .smtp_port = env_port("SMTP_PORT", 587),
      .smtp_username = env_or("SMTP_USERNAME", ""),
      .smtp_password = env_or("SMTP_PASSWORD", ""),
      .smtp_from = env_or("SMTP_FROM", "SimTrade <no-reply@localhost>"),
      .smtp_use_tls = env_boolean("SMTP_USE_TLS", true),
      .otp_ttl_seconds = env_positive_integer("OTP_TTL_SECONDS", 600),
      .otp_pepper = env_or("OTP_PEPPER", "development-only-change-me"),
      .access_token_ttl_seconds = env_positive_integer("ACCESS_TOKEN_TTL_SECONDS", 900),
  };
  if (config.alpaca_max_stream_symbols != 30) {
    throw std::runtime_error("ALPACA_MAX_STREAM_SYMBOLS must be 30 for the Phase 2 five-pinned/25-dynamic allocation");
  }
  return config;
}

}  // namespace simtrade::config
