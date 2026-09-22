#include "api/HttpServer.hpp"
#include "cache/RedisClient.hpp"
#include "config/Config.hpp"
#include "database/PostgresRepository.hpp"

#include <boost/asio/io_context.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <iostream>
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

    boost::asio::io_context io(static_cast<int>(config.worker_threads));
    simtrade::api::HttpServer server(io, config, postgres, redis);
    server.run();

    std::cout << nlohmann::json({
      {"level", "info"},
      {"event", "server_started"},
      {"host", config.http_host},
      {"port", config.http_port},
      {"threads", config.worker_threads},
      {"postgres", postgres.healthy()},
      {"redis", redis.healthy()}
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
