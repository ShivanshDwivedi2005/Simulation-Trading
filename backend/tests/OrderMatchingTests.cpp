#include "order/MatchingEngine.hpp"
#include "order/RedisOrderStore.hpp"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace {

using namespace simtrade::order;

int failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

std::string timestamp(std::chrono::system_clock::time_point time = std::chrono::system_clock::now()) {
  const auto value = std::chrono::system_clock::to_time_t(time);
  std::tm utc{};
  gmtime_r(&value, &utc);
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

class RedisFixture {
 public:
  explicit RedisFixture(const std::string& url) {
    const std::regex pattern(R"(^redis://(?:([^@/]*)@)?([^:/]+)(?::([0-9]+))?(?:/([0-9]+))?$)");
    std::smatch match;
    if (!std::regex_match(url, match, pattern)) throw std::runtime_error("invalid test Redis URL");
    const auto host = match[2].str();
    const auto port = match[3].matched ? std::stoi(match[3].str()) : 6379;
    const auto database = match[4].matched ? std::stoi(match[4].str()) : 0;
    context_ = redisConnect(host.c_str(), port);
    if (context_ == nullptr || context_->err != 0) throw std::runtime_error("test Redis connection failed");
    if (match[1].matched) {
      const auto credentials = match[1].str();
      const auto separator = credentials.find(':');
      const auto password = separator == std::string::npos ? credentials : credentials.substr(separator + 1);
      release(redisCommand(context_, "AUTH %b", password.data(), password.size()));
    }
    release(redisCommand(context_, "SELECT %d", database));
    flush();
  }

  ~RedisFixture() {
    if (context_ != nullptr) {
      flush();
      redisFree(context_);
    }
  }

  void flush() { release(redisCommand(context_, "FLUSHDB")); }

  [[nodiscard]] bool set_contains(const std::string& key, const std::string& value) {
    auto* reply = static_cast<redisReply*>(redisCommand(context_, "SISMEMBER %b %b", key.data(), key.size(), value.data(), value.size()));
    const bool found = reply != nullptr && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
    release(reply);
    return found;
  }

  [[nodiscard]] bool execution_event_has_required_fields(const std::string& execution_id) {
    auto* reply = static_cast<redisReply*>(redisCommand(context_, "XRANGE stream:order_events - +"));
    if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) { release(reply); return false; }
    const std::vector<std::string> required{
        "event_id", "execution_id", "order_id", "trader_id", "instrument_id", "filled_quantity",
        "remaining_quantity", "execution_price_ticks", "market_bid_ticks", "market_ask_ticks",
        "price_source", "market_timestamp", "execution_timestamp"};
    bool found = false;
    for (std::size_t index = 0; index < reply->elements && !found; ++index) {
      const auto* entry = reply->element[index];
      if (entry == nullptr || entry->type != REDIS_REPLY_ARRAY || entry->elements != 2) continue;
      const auto* fields = entry->element[1];
      if (fields == nullptr || fields->type != REDIS_REPLY_ARRAY) continue;
      std::vector<std::string> names;
      std::string current_execution;
      for (std::size_t field = 0; field + 1 < fields->elements; field += 2) {
        const std::string name(fields->element[field]->str, static_cast<std::size_t>(fields->element[field]->len));
        const std::string value(fields->element[field + 1]->str,
                                static_cast<std::size_t>(fields->element[field + 1]->len));
        names.push_back(name);
        if (name == "execution_id") current_execution = value;
      }
      found = current_execution == execution_id &&
              std::all_of(required.begin(), required.end(), [&](const auto& name) {
                return std::find(names.begin(), names.end(), name) != names.end();
              });
    }
    release(reply);
    return found;
  }

 private:
  static void release(void* value) {
    if (value != nullptr) freeReplyObject(value);
  }

  redisContext* context_{nullptr};
};

Order make_order(OrderId id, TraderId trader, InstrumentId instrument, Side side, OrderType type,
                 Quantity quantity, std::uint64_t sequence, std::optional<Price> limit = std::nullopt,
                 std::optional<Price> stop = std::nullopt) {
  return Order{id, "client-" + std::to_string(id), trader, instrument, side, type, OrderStatus::Open,
               limit, stop, quantity, quantity, sequence, 1, std::nullopt,
               std::chrono::system_clock::now(), std::chrono::system_clock::now()};
}

Instrument instrument() {
  return Instrument{1, "AAPL", "Apple Inc.", "0.01", true, 1};
}

nlohmann::json quote(double bid, double ask, double bid_size, double ask_size,
                     const std::string& market_timestamp = timestamp()) {
  return {{"type", "quote"}, {"symbol", "AAPL"}, {"bidPrice", bid}, {"askPrice", ask},
          {"bidSize", bid_size}, {"askSize", ask_size}, {"source", "mock"},
          {"timestamp", market_timestamp}};
}

nlohmann::json trade(double price, const std::string& market_timestamp = timestamp()) {
  return {{"type", "trade"}, {"symbol", "AAPL"}, {"price", price}, {"size", 100},
          {"source", "mock"}, {"timestamp", market_timestamp}};
}

void test_insertion_cancellation_and_reconnect(const std::string& url, RedisFixture& fixture) {
  fixture.flush();
  RedisOrderStore store(url, 30000);
  const auto first = make_order(101, 7, 1, Side::Buy, OrderType::Limit, 10, 1, 20000);
  const auto second = make_order(102, 7, 1, Side::Buy, OrderType::Limit, 10, 2, 20000);
  check(store.add_order(first) && store.add_order(second), "active orders are inserted atomically");
  check(store.find_order(first.id)->remainingQuantity == 10, "the active-order hash stores current quantity");
  check(fixture.set_contains("trader:7:open_orders", "101"), "the trader open-order set is populated");
  check(store.cancel_order(first.id, first.traderId, first.version, "cancel-101"),
        "cancellation succeeds for the exact active member");
  const auto indexed = store.indexed_orders(RedisOrderStore::limit_book_key(1, Side::Buy));
  check(indexed == std::vector<OrderId>{102}, "exact cancellation leaves equal-price neighbors indexed");
  check(!store.cancel_order(first.id, first.traderId, first.version, "cancel-101-again"),
        "terminal orders cannot be cancelled twice");
  store.reset_connection();
  check(store.healthy() && store.find_order(second.id).has_value(), "the persistent client reconnects after connection loss");
}

void test_modification_and_priority(const std::string& url, RedisFixture& fixture) {
  fixture.flush();
  RedisOrderStore store(url, 30000);
  auto order = make_order(201, 8, 1, Side::Buy, OrderType::Limit, 10, 5, 19800);
  check(store.add_order(order), "order is available for modification");
  check(store.modify_quantity(order.id, order.traderId, 8, 99, 1, "reduce-201"),
        "quantity reduction succeeds");
  auto reduced = store.find_order(order.id).value();
  check(reduced.quantity == 8 && reduced.sequenceNumber == 5 && reduced.version == 2,
        "quantity reduction keeps time priority");
  check(store.modify_quantity(order.id, order.traderId, 12, 20, 2, "increase-201"),
        "quantity increase succeeds");
  auto increased = store.find_order(order.id).value();
  check(increased.quantity == 12 && increased.sequenceNumber == 20 && increased.version == 3,
        "quantity increase receives new sequence priority");
  check(store.modify_price(order.id, order.traderId, 20200, std::nullopt, 30, 3, "price-201"),
        "price modification succeeds");
  auto repriced = store.find_order(order.id).value();
  check(repriced.limitPrice == 20200 && repriced.sequenceNumber == 30 && repriced.version == 4,
        "price modification assigns a new sequence");

  check(store.add_order(make_order(202, 8, 1, Side::Buy, OrderType::Limit, 1, 2, 20000)),
        "lower price order is added");
  check(store.add_order(make_order(203, 8, 1, Side::Buy, OrderType::Limit, 1, 1, 20200)),
        "equal price order with earlier sequence is added");
  const auto prioritized = store.indexed_orders(RedisOrderStore::limit_book_key(1, Side::Buy));
  check(prioritized.size() == 3 && prioritized[0] == 203 && prioritized[1] == 201 && prioritized[2] == 202,
        "price priority and FIFO ordering are preserved by scores and padded members");
}

void test_limit_market_and_partial_fills(const std::string& url, RedisFixture& fixture) {
  fixture.flush();
  RedisOrderStore store(url, 30000);
  std::vector<Execution> executions;
  MatchingEngine engine(store, {instrument()}, 100,
                        [&](const Execution& execution, std::uint32_t) { executions.push_back(execution); });
  check(store.add_order(make_order(301, 9, 1, Side::Buy, OrderType::Limit, 2, 1, 20000)), "buy 200 added");
  check(store.add_order(make_order(302, 9, 1, Side::Buy, OrderType::Limit, 2, 2, 19800)), "buy 198 added");
  check(store.add_order(make_order(303, 9, 1, Side::Buy, OrderType::Limit, 2, 3, 19500)), "buy 195 added");
  const auto buy_result = engine.process(quote(196.0, 197.0, 10, 10));
  check(buy_result.fills == 2 && !store.find_order(301) && !store.find_order(302) && store.find_order(303),
        "limit buys match only the crossed price range");

  check(store.add_order(make_order(304, 9, 1, Side::Sell, OrderType::Limit, 2, 4, 19000)), "sell 190 added");
  check(store.add_order(make_order(305, 9, 1, Side::Sell, OrderType::Limit, 2, 5, 19300)), "sell 193 added");
  check(store.add_order(make_order(306, 9, 1, Side::Sell, OrderType::Limit, 2, 6, 19600)), "sell 196 added");
  const auto sell_result = engine.process(quote(194.0, 205.0, 10, 10));
  check(sell_result.fills == 2 && !store.find_order(304) && !store.find_order(305) && store.find_order(306),
        "limit sells match only the crossed price range");

  check(store.add_order(make_order(307, 9, 1, Side::Buy, OrderType::Market, 3, 7)), "market order added");
  check(engine.process(quote(195.0, 200.0, 10, 10)).fills == 1 && !store.find_order(307),
        "market buys execute at a fresh ask");

  check(store.add_order(make_order(308, 9, 1, Side::Buy, OrderType::Limit, 10, 8, 20500)), "partial-fill order added");
  check(engine.process(quote(195.0, 201.0, 10, 4)).fills == 1, "available size produces a partial fill");
  const auto partial = store.find_order(308).value();
  check(partial.remainingQuantity == 6 && partial.status == OrderStatus::PartiallyFilled,
        "partial fill retains the order with reduced quantity");
  check(engine.process(quote(195.0, 201.0, 10, 6)).fills == 1 && !store.find_order(308),
        "a later quote completes the remaining quantity exactly once");
  check(executions.size() == 7, "each successful fill emits one execution callback");
}

void test_stops_and_stop_limits(const std::string& url, RedisFixture& fixture) {
  fixture.flush();
  RedisOrderStore store(url, 30000);
  MatchingEngine engine(store, {instrument()}, 100);
  static_cast<void>(engine.process(quote(100.0, 101.0, 20, 20)));
  check(store.add_order(make_order(401, 10, 1, Side::Buy, OrderType::Stop, 2, 1, std::nullopt, 10000)),
        "buy stop added");
  const auto buy_stop = engine.process(trade(100.5));
  check(buy_stop.activations == 1 && buy_stop.fills == 1 && !store.find_order(401),
        "buy stop activates on a trade at or above its stop and fills from the ask");

  check(store.add_order(make_order(402, 10, 1, Side::Sell, OrderType::Stop, 2, 2, std::nullopt, 9950)),
        "sell stop added");
  const auto sell_stop = engine.process(trade(99.0));
  check(sell_stop.activations == 1 && sell_stop.fills == 1 && !store.find_order(402),
        "sell stop activates on a trade at or below its stop and fills from the bid");

  fixture.flush();
  RedisOrderStore transition_store(url, 30000);
  MatchingEngine transition_engine(transition_store, {instrument()}, 100);
  static_cast<void>(transition_engine.process(quote(100.0, 101.0, 20, 20)));
  check(transition_store.add_order(make_order(403, 10, 1, Side::Buy, OrderType::StopLimit, 2, 3, 9900, 10000)),
        "stop-limit order added");
  const auto transition = transition_engine.process(trade(100.5));
  check(transition.activations == 1 && transition.fills == 0 && transition_store.find_order(403),
        "stop-limit activation does not fill when its limit is not crossed");
  check(transition_store.eligible_stop_orders(1, Side::Buy, 20000, 10).empty() &&
            transition_store.indexed_orders(RedisOrderStore::limit_book_key(1, Side::Buy)) == std::vector<OrderId>{403},
        "stop-limit activation atomically leaves the stop index and enters the limit index");
  const auto activated_order = transition_store.find_order(403).value();
  fixture.flush();
  const auto recovery = transition_store.recover({activated_order}, {instrument()});
  check(recovery.rebuilt == 1 &&
            transition_store.indexed_orders(RedisOrderStore::limit_book_key(1, Side::Buy)) == std::vector<OrderId>{403},
        "recovery preserves an activated stop-limit in the limit index");
  check(transition_engine.process(quote(98.0, 99.0, 20, 20)).fills == 1 && !transition_store.find_order(403),
        "an activated stop-limit fills on a later crossed quote");
}

void test_stale_and_duplicate_protection(const std::string& url, RedisFixture& fixture) {
  fixture.flush();
  RedisOrderStore store(url, 1000);
  MatchingEngine engine(store, {instrument()}, 100);
  check(store.add_order(make_order(501, 11, 1, Side::Buy, OrderType::Market, 1, 1)), "stale-price order added");
  const auto stale_time = timestamp(std::chrono::system_clock::now() - std::chrono::seconds(10));
  const auto stale = engine.process(quote(100.0, 101.0, 10, 10, stale_time));
  check(stale.fills == 0 && stale.staleRejections == 1 && store.find_order(501),
        "stale quotes cannot fill market orders");

  fixture.flush();
  RedisOrderStore duplicate_store(url, 30000);
  const auto order = make_order(502, 11, 1, Side::Buy, OrderType::Limit, 10, 2, 10500);
  check(duplicate_store.add_order(order), "duplicate-fill order added");
  const auto market_time = timestamp();
  const auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const MarketSnapshot snapshot{1, 10000, 10100, 10, 10, 10050, "mock", market_time, time_ms, time_ms};
  const FillRequest first{"event-502", "execution-502", 502, 1, 3, snapshot, time_ms};
  const auto applied = duplicate_store.apply_fill(first);
  const auto stream_length = duplicate_store.stream_length();
  auto duplicate = first;
  duplicate.expectedVersion = 2;
  const auto replay = duplicate_store.apply_fill(duplicate);
  check(applied.applied && replay.duplicate && duplicate_store.find_order(502)->remainingQuantity == 7,
        "execution identifiers prevent duplicate partial fills");
  check(duplicate_store.stream_length() == stream_length, "duplicate fills do not publish duplicate events");
  check(fixture.execution_event_has_required_fields("execution-502"),
        "execution stream events contain every required persistence field");

  const auto terminal_order = make_order(503, 11, 1, Side::Buy, OrderType::Limit, 1, 3, 10500);
  check(duplicate_store.add_order(terminal_order), "terminal-recovery order added");
  const FillRequest terminal_fill{"event-503", "execution-503", 503, 1, 1, snapshot, time_ms};
  check(duplicate_store.apply_fill(terminal_fill).applied, "terminal-recovery order fills");
  const auto recovery = duplicate_store.recover({terminal_order}, {instrument()});
  check(recovery.invalid == 1 && !duplicate_store.find_order(503),
        "a persisted terminal marker prevents a completed order from being rebuilt and filled twice");
}

void test_recovery_and_bounded_batches(const std::string& url, RedisFixture& fixture) {
  fixture.flush();
  RedisOrderStore store(url, 30000);
  const auto valid = make_order(601, 12, 1, Side::Buy, OrderType::Limit, 1, 1, 20000);
  const auto stale = make_order(602, 12, 1, Side::Sell, OrderType::Limit, 1, 2, 20100);
  check(store.add_order(stale), "stale Redis-only order added");
  const auto recovered = store.recover({valid}, {instrument()});
  check(recovered.rebuilt == 1 && recovered.removed == 1 && store.find_order(601) && !store.find_order(602),
        "startup recovery rebuilds PostgreSQL active orders and removes stale Redis orders");
  const auto repeated = store.recover({valid}, {instrument()});
  check(repeated.rebuilt == 1 && repeated.removed == 0,
        "active-index reconstruction is idempotent");

  fixture.flush();
  RedisOrderStore batch_store(url, 30000);
  MatchingEngine engine(batch_store, {instrument()}, 100);
  for (OrderId id = 700; id < 850; ++id) {
    check(batch_store.add_order(make_order(id, 13, 1, Side::Buy, OrderType::Limit, 1, id, 20000)),
          "batch order inserted");
  }
  const auto matched = engine.process(quote(199.0, 200.0, 200, 200));
  check(matched.fills == 150 && matched.batches == 2 && matched.maximumBatchSize == 100,
        "eligible orders are processed across bounded batches");
  check(batch_store.indexed_orders(RedisOrderStore::limit_book_key(1, Side::Buy)).empty(),
        "bounded processing continues until the crossed range is empty");
}

}  // namespace

int main() {
  const auto* redis_url = std::getenv("SIMTRADE_TEST_REDIS_URL");
  if (redis_url == nullptr || std::string(redis_url).empty()) {
    std::cout << "Redis order tests skipped because SIMTRADE_TEST_REDIS_URL is not set.\n";
    return EXIT_SUCCESS;
  }
  try {
    RedisFixture fixture(redis_url);
    test_insertion_cancellation_and_reconnect(redis_url, fixture);
    test_modification_and_priority(redis_url, fixture);
    test_limit_market_and_partial_fills(redis_url, fixture);
    test_stops_and_stop_limits(redis_url, fixture);
    test_stale_and_duplicate_protection(redis_url, fixture);
    test_recovery_and_bounded_batches(redis_url, fixture);
  } catch (const std::exception& exception) {
    ++failures;
    std::cerr << "FAIL: Redis order test setup failed: " << exception.what() << '\n';
  }
  if (failures == 0) {
    std::cout << "All Redis order tests passed.\n";
    return EXIT_SUCCESS;
  }
  std::cerr << failures << " Redis order test(s) failed.\n";
  return EXIT_FAILURE;
}
