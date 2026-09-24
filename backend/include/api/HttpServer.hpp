#pragma once

#include "cache/RedisClient.hpp"
#include "config/Config.hpp"
#include "database/PostgresRepository.hpp"
#include "market/AlpacaMarketDataStream.hpp"
#include "market/InstrumentCatalogue.hpp"
#include "market/MarketDataHub.hpp"
#include "market/MarketDataRestClient.hpp"
#include "market/SubscriptionManager.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace simtrade::api {

class HttpServer {
 public:
  HttpServer(boost::asio::io_context& io,
             const config::Config& config,
             database::PostgresRepository& postgres,
             const cache::RedisClient& redis,
             market::InstrumentCatalogue& catalogue,
             market::AlpacaMarketDataStream& market_stream,
             market::MarketDataHub& market_hub,
             market::SubscriptionManager& subscription_manager,
             market::MarketDataRestClient& market_rest_client);
  void run();
  void stop();

 private:
  void accept();

  boost::asio::io_context& io_;
  const config::Config& config_;
  database::PostgresRepository& postgres_;
  const cache::RedisClient& redis_;
  market::InstrumentCatalogue& catalogue_;
  market::AlpacaMarketDataStream& market_stream_;
  market::MarketDataHub& market_hub_;
  market::SubscriptionManager& subscription_manager_;
  market::MarketDataRestClient& market_rest_client_;
  boost::asio::ip::tcp::acceptor acceptor_;
};

}  // namespace simtrade::api
