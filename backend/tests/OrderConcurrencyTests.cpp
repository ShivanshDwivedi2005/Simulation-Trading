#include "concurrency/BoundedQueue.hpp"
#include "order/OrderEventWorkers.hpp"
#include "order/OrderProcessingRuntime.hpp"
#include "order/RedisOrderStore.hpp"
#include "order/RedisStreamConsumer.hpp"

#include <hiredis/hiredis.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace simtrade::order;

void check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<Instrument> instruments() {
  std::vector<Instrument> values;
  for (std::uint32_t id = 1; id <= 30; ++id) {
    std::ostringstream symbol;
    symbol << 'S' << std::setw(2) << std::setfill('0') << id;
    values.push_back({id, symbol.str(), "Instrument " + std::to_string(id), "0.01", true,
                      static_cast<std::uint8_t>(id <= 15 ? 1 : 2)});
  }
  return values;
}

std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm value{};
  gmtime_r(&time, &value);
  std::ostringstream output;
  output << std::put_time(&value, "%Y-%m-%dT%H:%M:%S.000Z");
  return output.str();
}

Order make_order(OrderId id, InstrumentId instrument_id, std::uint64_t sequence) {
  const auto now = std::chrono::system_clock::now();
  return {id, "load-" + std::to_string(id), 100000 + id, instrument_id, Side::Buy,
          OrderType::Limit, OrderStatus::Open, 110, std::nullopt, 1, 1, sequence, 1,
          std::nullopt, now, now, false};
}

void flush_redis(const std::string& url) {
  auto value = url.substr(8);
  const auto slash = value.find('/');
  const auto database = slash == std::string::npos ? 0 : std::stoi(value.substr(slash + 1));
  auto authority = value.substr(0, slash);
  const auto at = authority.rfind('@');
  if (at != std::string::npos) authority.erase(0, at + 1);
  const auto colon = authority.rfind(':');
  const auto host = colon == std::string::npos ? authority : authority.substr(0, colon);
  const auto port = colon == std::string::npos ? 6379 : std::stoi(authority.substr(colon + 1));
  auto* context = redisConnect(host.c_str(), port);
  if (context == nullptr || context->err != 0) throw std::runtime_error("test Redis connection failed");
  auto* select = static_cast<redisReply*>(redisCommand(context, "SELECT %d", database));
  if (select != nullptr) freeReplyObject(select);
  auto* flush = static_cast<redisReply*>(redisCommand(context, "FLUSHDB"));
  if (flush == nullptr || flush->type == REDIS_REPLY_ERROR) {
    if (flush != nullptr) freeReplyObject(flush);
    redisFree(context);
    throw std::runtime_error("test Redis flush failed");
  }
  freeReplyObject(flush);
  redisFree(context);
}

void test_router_and_queue() {
  InstrumentRouter router(instruments());
  check(router.group_size(1) == 15 && router.group_size(2) == 15, "matching groups own 15 instruments each");
  for (std::uint32_t id = 1; id <= 15; ++id) check(router.group_for(id) == 1, "instrument 1-15 routing");
  for (std::uint32_t id = 16; id <= 30; ++id) check(router.group_for(id) == 2, "instrument 16-30 routing");

  const std::array<std::string, 30> selected_symbols{
      "NVDA", "AAPL", "MSFT", "TSLA", "AMZN", "GOOGL", "META", "AVGO", "AMD", "MU",
      "ORCL", "PLTR", "NFLX", "WMT", "COST", "HD", "JPM", "BAC", "V", "MA",
      "LLY", "JNJ", "UNH", "ABBV", "XOM", "CVX", "GE", "CAT", "BA", "PG"};
  std::vector<Instrument> selected_instruments;
  for (std::size_t index = 0; index < selected_symbols.size(); ++index) {
    selected_instruments.push_back({static_cast<InstrumentId>(index + 1), selected_symbols[index],
                                    selected_symbols[index], "0.01", true,
                                    static_cast<std::uint8_t>(index < 15 ? 1 : 2)});
  }
  InstrumentRouter selected_router(std::move(selected_instruments));
  for (std::size_t index = 0; index < selected_symbols.size(); ++index) {
    check(selected_router.group_for_symbol(selected_symbols[index]) == (index < 15 ? 1 : 2),
          "selected stock routes to its assigned matching worker");
  }

  simtrade::concurrency::BoundedQueue<int> queue(1);
  check(queue.push(1, std::chrono::milliseconds(1)), "first queue item accepted");
  check(!queue.push(2, std::chrono::milliseconds(5)), "bounded queue applies backpressure");
  int value = 0;
  check(queue.pop(value) && value == 1, "queue preserves FIFO order");
  check(queue.push(2, std::chrono::milliseconds(1)), "queue accepts after capacity is released");
  queue.close();
  check(queue.pop(value) && value == 2 && !queue.pop(value), "closed queue drains before stopping");
}

void test_stream_recovery(const std::string& redis_url) {
  RedisOrderStore store(redis_url, 30000);
  RedisStreamConsumer first(redis_url, "recovery_test", "consumer-a");
  first.ensure_group();
  check(store.add_order(make_order(900001, 1, 900001)), "recovery fixture order added");
  const auto read = first.read(10, std::chrono::milliseconds(10));
  check(!read.empty(), "stream event delivered to first consumer");
  RedisStreamConsumer second(redis_url, "recovery_test", "consumer-b");
  second.ensure_group();
  const auto recovered = second.recover(10, std::chrono::milliseconds(0));
  check(!recovered.empty() && recovered.front().streamId == read.front().streamId,
        "pending stream event recovered after consumer restart");
  second.acknowledge({recovered.front().streamId});
  check(second.pending_count() == 0, "recovered event acknowledged");
}

void test_concurrent_runtime(const std::string& redis_url) {
  flush_redis(redis_url);
  OrderProcessingRuntime runtime(redis_url, instruments(), 30000, 100, 4096);
  runtime.start();
  check(runtime.matching_worker_count() == 2, "exactly two matching workers are configured");
  PersistenceWorker unavailable_persistence(
      redis_url, "postgresql://invalid:invalid@127.0.0.1:1/invalid?connect_timeout=1", 100);
  unavailable_persistence.start();

  const auto cancelled = make_order(800001, 1, 800001);
  check(runtime.route_order(cancelled, "add:800001", std::chrono::seconds(1)) == EnqueueResult::Accepted,
        "cancellation fixture accepted");
  CancelOrderCommand cancel{cancelled.id, cancelled.traderId, cancelled.version, "cancel:800001:1"};
  check(runtime.route_cancel(cancelled.instrumentId, cancel, cancel.eventId, std::chrono::seconds(1)) ==
            EnqueueResult::Accepted,
        "cancellation is sequenced through the owning worker");

  const auto started = std::chrono::steady_clock::now();
  constexpr std::uint64_t order_count = 600;
  for (std::uint64_t index = 0; index < order_count; ++index) {
    const auto instrument_id = static_cast<InstrumentId>((index % 30) + 1);
    const auto order = make_order(index + 1, instrument_id, index + 1);
    check(runtime.route_order(order, "add:" + std::to_string(order.id), std::chrono::seconds(1)) ==
              EnqueueResult::Accepted,
          "accepted orders are retained");
  }

  std::atomic<bool> producers_ready{false};
  auto producer = [&](std::uint32_t first, std::uint32_t last) {
    while (!producers_ready.load()) std::this_thread::yield();
    for (auto id = first; id <= last; ++id) {
      std::ostringstream symbol;
      symbol << 'S' << std::setw(2) << std::setfill('0') << id;
      check(runtime.route_market({{"type", "quote"}, {"symbol", symbol.str()},
                                  {"bidPrice", 0.99}, {"askPrice", 1.00},
                                  {"bidSize", 1000}, {"askSize", 1000},
                                  {"source", "mock"}, {"timestamp", timestamp()}},
                                 std::chrono::seconds(1)) == EnqueueResult::Accepted,
            "simultaneous market event accepted");
    }
  };
  std::thread group1(producer, 1, 15);
  std::thread group2(producer, 16, 30);
  producers_ready = true;
  group1.join();
  group2.join();
  check(runtime.wait_until_idle(std::chrono::seconds(10)), "runtime drains concurrent work");

  const auto duplicate_time = timestamp();
  nlohmann::json duplicate{{"type", "quote"}, {"symbol", "S01"}, {"bidPrice", 0.99},
                           {"askPrice", 1.00}, {"bidSize", 1}, {"askSize", 1},
                           {"source", "mock"}, {"timestamp", duplicate_time}};
  check(runtime.route_market(duplicate, std::chrono::seconds(1)) == EnqueueResult::Accepted,
        "first duplicate fixture accepted");
  check(runtime.route_market(duplicate, std::chrono::seconds(1)) == EnqueueResult::Accepted,
        "duplicate command safely queued");
  check(runtime.wait_until_idle(std::chrono::seconds(2)), "duplicate commands drained");

  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  const auto health = runtime.health();
  check(health.worker1Healthy && health.worker2Healthy, "both matching workers run concurrently");
  check(health.metrics.fillsProcessed == order_count,
        "hundreds of eligible orders fill once; observed " + std::to_string(health.metrics.fillsProcessed));
  check(health.metrics.duplicateCommands >= 1, "duplicate market commands are ignored");
  check(health.metrics.failures == 0, "concurrent execution has no command failures");
  check(unavailable_persistence.health().retryCount > 0,
        "PostgreSQL failure is retried without blocking matching");
  RedisOrderStore inspection(redis_url, 30000);
  check(!inspection.find_order(cancelled.id), "cancel precedes later prices for the same instrument");

  std::cout << nlohmann::json({{"orders_processed_per_second", order_count / elapsed},
                               {"fills_processed_per_second", static_cast<double>(health.metrics.fillsProcessed) / elapsed},
                               {"average_matching_latency_us", health.metrics.averageLatencyMicros},
                               {"p95_matching_latency_us", health.metrics.p95LatencyMicros},
                               {"p99_matching_latency_us", health.metrics.p99LatencyMicros},
                               {"maximum_queue_depth", health.metrics.maximumQueueDepth},
                               {"postgres_batch_size", 100}}).dump() << std::endl;

  for (std::uint64_t index = 0; index < 100; ++index) {
    const auto order = make_order(700001 + index, static_cast<InstrumentId>((index % 30) + 1), 700001 + index);
    check(runtime.route_order(order, "shutdown:" + std::to_string(order.id), std::chrono::seconds(1)) ==
              EnqueueResult::Accepted,
          "shutdown fixture accepted");
  }
  runtime.stop();
  unavailable_persistence.stop(false);
  check(runtime.health().metrics.commandsProcessed >= order_count + 132,
        "graceful shutdown processes queued commands");
}

}  // namespace

int main() {
  try {
    test_router_and_queue();
    const auto* redis_url = std::getenv("SIMTRADE_TEST_REDIS_URL");
    if (redis_url == nullptr || *redis_url == '\0') {
      std::cout << "Phase 3 Redis tests skipped: SIMTRADE_TEST_REDIS_URL is not set" << std::endl;
      return 0;
    }
    test_stream_recovery(redis_url);
    test_concurrent_runtime(redis_url);
    flush_redis(redis_url);
    std::cout << "Phase 3 concurrency tests passed" << std::endl;
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "Phase 3 concurrency test failed: " << exception.what() << std::endl;
    return 1;
  }
}
