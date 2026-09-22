#pragma once

#include "cache/RedisClient.hpp"
#include "config/Config.hpp"
#include "database/PostgresRepository.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace simtrade::api {

class HttpServer {
 public:
  HttpServer(boost::asio::io_context& io,
             const config::Config& config,
             database::PostgresRepository& postgres,
             const cache::RedisClient& redis);
  void run();

 private:
  void accept();

  boost::asio::io_context& io_;
  const config::Config& config_;
  database::PostgresRepository& postgres_;
  const cache::RedisClient& redis_;
  boost::asio::ip::tcp::acceptor acceptor_;
};

}  // namespace simtrade::api
