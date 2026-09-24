#include "api/HttpServer.hpp"
#include "cache/RedisClient.hpp"
#include "config/Config.hpp"
#include "database/PostgresRepository.hpp"
#include "market/AlpacaMarketDataStream.hpp"
#include "market/InstrumentCatalogue.hpp"
#include "market/MarketDataHub.hpp"
#include "market/SubscriptionManager.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <chrono>
#include <csignal>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

class CurlGlobal {
 public:
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("failed to initialize network client");
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

}  // namespace

int main() {
  try {
    const CurlGlobal curl_global;
    const auto config = simtrade::config::Config::from_environment();
    simtrade::database::PostgresRepository postgres(config.database_url);
    simtrade::cache::RedisClient redis(config.redis_url);
    simtrade::market::InstrumentCatalogue catalogue(config, redis);
    simtrade::market::AlpacaMarketDataStream market_stream(config, redis);
    catalogue.start();
    simtrade::market::SubscriptionManager subscription_manager(
        market_stream,
        [&catalogue](const std::string& symbol) { return catalogue.contains(symbol); },
        config.alpaca_pinned_symbols,
        config.alpaca_max_stream_symbols,
        std::chrono::seconds(config.market_data_eviction_grace_seconds),
        std::chrono::seconds(config.market_data_min_residency_seconds));
    if (postgres.healthy()) {
      subscription_manager.restore_pending_orders(postgres.pending_order_symbol_counts());
    } else {
      std::cerr << nlohmann::json({{"level", "warning"},
                                   {"event", "pending_order_protection_not_restored"},
                                   {"message", postgres.status()}}).dump() << std::endl;
    }
    simtrade::market::MarketDataHub market_hub(
        subscription_manager,
        [&redis](const std::string& key) { return redis.get(key); });
    subscription_manager.set_status_handler(
        [&market_hub](const std::string& symbol,
                      const std::string& status,
                      bool live,
                      std::optional<std::size_t> queue_position,
                      const std::string& message) {
          market_hub.publish_status(symbol, status, live, queue_position, message);
        });
    market_stream.set_subscription_handlers(
        [&subscription_manager](const std::set<std::string>& confirmed) {
          subscription_manager.on_confirmation(confirmed);
        },
        [&subscription_manager](bool connected) {
          subscription_manager.on_connection_changed(connected);
        });

    market_stream.start([&market_hub](const nlohmann::json& event) { market_hub.publish(event); });

    boost::asio::io_context io(static_cast<int>(config.worker_threads));
    simtrade::api::HttpServer server(io, config, postgres, redis, catalogue, market_stream, market_hub,
                                     subscription_manager);
    server.run();
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
      server.stop();
      market_stream.stop();
      catalogue.stop();
      io.stop();
    });

    std::cout << nlohmann::json({
      {"level", "info"},
      {"event", "server_started"},
      {"host", config.http_host},
      {"port", config.http_port},
      {"threads", config.worker_threads},
      {"postgres", postgres.healthy()},
      {"redis", redis.healthy()},
      {"market_data_websocket", config.alpaca_data_ws_url},
      {"maximum_stream_symbols", config.alpaca_max_stream_symbols}
    }).dump() << std::endl;

    std::vector<std::thread> workers;
    workers.reserve(config.worker_threads > 0 ? config.worker_threads - 1 : 0);
    for (std::size_t index = 1; index < config.worker_threads; ++index) workers.emplace_back([&io] { io.run(); });
    io.run();
    for (auto& worker : workers) worker.join();
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << nlohmann::json({{"level", "critical"}, {"event", "startup_failed"}, {"message", exception.what()}}).dump() << std::endl;
    return 1;
  }
}
