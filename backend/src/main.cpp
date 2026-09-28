#include "api/HttpServer.hpp"
#include "cache/RedisClient.hpp"
#include "config/Config.hpp"
#include "database/PostgresRepository.hpp"
#include "market/AlpacaMarketDataStream.hpp"
#include "market/InstrumentCatalogue.hpp"
#include "market/MarketDataHub.hpp"
#include "market/MarketDataRestClient.hpp"
#include "market/SubscriptionManager.hpp"
#include "order/MatchingEngine.hpp"
#include "order/OrderEventWorkers.hpp"
#include "order/OrderProcessingRuntime.hpp"
#include "order/OrderRepository.hpp"
#include "order/RedisOrderStore.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <chrono>
#include <csignal>
#include <memory>
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
    simtrade::order::RedisOrderStore order_store(config.redis_url, config.order_maximum_price_age_ms);
    std::unique_ptr<simtrade::order::OrderRepository> order_repository;
    std::vector<simtrade::order::Instrument> order_instruments;
    if (postgres.healthy()) {
      order_repository = std::make_unique<simtrade::order::OrderRepository>(config.database_url);
      order_instruments = order_repository->load_active_instruments();
    }
    simtrade::order::MatchingEngine recovery_engine(
        order_store, order_instruments, config.order_matching_batch_size);
    if (order_repository && order_store.healthy()) {
      const auto recovery = recovery_engine.recover(order_repository->load_active_orders());
      std::cout << nlohmann::json({{"level", "info"},
                                   {"event", "active_order_recovery_completed"},
                                   {"rebuilt", recovery.rebuilt},
                                   {"removed", recovery.removed},
                                   {"invalid", recovery.invalid}}).dump() << std::endl;
    }
    simtrade::market::InstrumentCatalogue catalogue(config, redis);
    simtrade::market::AlpacaMarketDataStream market_stream(config, redis);
    simtrade::market::MarketDataRestClient market_rest_client(config);
    catalogue.start(false);
    simtrade::market::SubscriptionManager subscription_manager(
        market_stream,
        [&catalogue](const std::string& symbol) { return catalogue.contains(symbol); },
        config.alpaca_pinned_symbols,
        config.alpaca_max_stream_symbols,
        std::chrono::seconds(config.market_data_eviction_grace_seconds),
        std::chrono::seconds(config.market_data_min_residency_seconds),
        std::chrono::steady_clock::now, false);
    if (postgres.healthy()) {
      subscription_manager.restore_pending_orders(postgres.pending_order_symbol_counts());
    } else {
      std::cerr << nlohmann::json({{"level", "warning"},
                                   {"event", "pending_order_protection_not_restored"},
                                   {"message", postgres.status()}}).dump() << std::endl;
    }
    simtrade::market::MarketDataHub market_hub(
        subscription_manager,
        [&redis](const std::string& key) { return redis.get(key); },
        [&market_rest_client](const std::string& symbol) {
          auto events = market_rest_client.latest_snapshots({symbol});
          auto bars = market_rest_client.historical_bars(symbol);
          bars["type"] = "historicalBars";
          events.push_back(std::move(bars));
          return events;
        });
    market_hub.set_authenticator(
        [&postgres](const std::string& token) { return postgres.trader_id_for_token(token); });
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

    simtrade::order::OrderProcessingRuntime order_runtime(
        config.redis_url, order_instruments, config.order_maximum_price_age_ms,
        config.order_matching_batch_size, config.order_queue_capacity);
    simtrade::order::NotificationWorker notification_worker(
        config.redis_url,
        [&market_hub](simtrade::order::TraderId trader_id, const nlohmann::json& event) {
          return market_hub.publish_order_event(trader_id, event) > 0;
        },
        config.order_persistence_batch_size);
    simtrade::order::PersistenceWorker persistence_worker(
        config.redis_url, config.database_url, config.order_persistence_batch_size);
    order_runtime.start();
    notification_worker.start();
    persistence_worker.start();

    market_stream.start([&market_hub, &order_runtime, &subscription_manager](const nlohmann::json& event) {
      const auto routed = order_runtime.route_market(event, std::chrono::hours(24));
      if (routed != simtrade::order::EnqueueResult::Accepted &&
          routed != simtrade::order::EnqueueResult::InvalidInstrument) {
        std::cerr << nlohmann::json({{"level", "warning"},
                                     {"event", "market_event_not_routed"},
                                     {"symbol", event.value("symbol", "")}}).dump() << std::endl;
      }
      subscription_manager.process_queue();
      market_hub.publish(event);
    });

    boost::asio::io_context io(1);
    simtrade::api::HttpServer server(io, config, postgres, redis, catalogue, market_stream, market_hub,
                                     subscription_manager, market_rest_client, order_repository.get(), order_runtime,
                                     notification_worker, persistence_worker);
    server.run();
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
      server.stop();
      market_stream.stop();
      order_runtime.stop_accepting();
      order_runtime.stop();
      persistence_worker.stop(true);
      notification_worker.stop();
      catalogue.stop();
      io.stop();
    });

    std::cout << nlohmann::json({
      {"level", "info"},
      {"event", "server_started"},
      {"host", config.http_host},
      {"port", config.http_port},
      {"threads", 6},
      {"matching_workers", order_runtime.matching_worker_count()},
      {"postgres", postgres.healthy()},
      {"redis", redis.healthy()},
      {"order_store", order_store.healthy()},
      {"market_data_websocket", config.alpaca_data_ws_url},
      {"maximum_stream_symbols", config.alpaca_max_stream_symbols}
    }).dump() << std::endl;

    io.run();
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << nlohmann::json({{"level", "critical"}, {"event", "startup_failed"}, {"message", exception.what()}}).dump() << std::endl;
    return 1;
  }
}
