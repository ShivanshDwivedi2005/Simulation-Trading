#include "market/MarketDataCore.hpp"
#include "market/MarketDataHub.hpp"
#include "market/SubscriptionManager.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

int failures = 0;

void check(bool condition, const std::string& description) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << description << '\n';
}

class FakeUpstream final : public simtrade::market::SymbolSubscriptionSink {
 public:
  explicit FakeUpstream(std::size_t maximum = 30) : registry(maximum) {}

  simtrade::market::SubscriptionResult subscribe(const std::string& symbol) override {
    std::scoped_lock lock(mutex);
    ++calls_in_progress;
    maximum_parallel_calls = std::max(maximum_parallel_calls.load(), calls_in_progress.load());
    requests.push_back(symbol);
    const auto result = registry.request(symbol);
    --calls_in_progress;
    return result;
  }

  bool unsubscribe(const std::string& symbol) override {
    std::scoped_lock lock(mutex);
    ++calls_in_progress;
    maximum_parallel_calls = std::max(maximum_parallel_calls.load(), calls_in_progress.load());
    releases.push_back(symbol);
    const bool result = registry.release(symbol);
    --calls_in_progress;
    return result;
  }

  simtrade::market::SubscriptionRegistry registry;
  std::mutex mutex;
  std::vector<std::string> requests;
  std::vector<std::string> releases;
  std::atomic<int> calls_in_progress{0};
  std::atomic<int> maximum_parallel_calls{0};
};

class FakeViewerSink final : public simtrade::market::ViewerSubscriptionSink {
 public:
  simtrade::market::WatchResult watch(const std::string& symbol) override {
    watches.push_back(symbol);
    return result;
  }
  simtrade::market::WatchResult refresh(const std::string& symbol) override {
    touches.push_back(symbol);
    return result;
  }
  void unwatch(const std::string& symbol) override { unwatches.push_back(symbol); }

  simtrade::market::WatchResult result{simtrade::market::WatchState::live, std::nullopt};
  std::vector<std::string> watches;
  std::vector<std::string> unwatches;
  std::vector<std::string> touches;
};

std::vector<std::string> pinned_symbols() { return {"PIN0", "PIN1", "PIN2", "PIN3", "PIN4"}; }

void confirm_desired(simtrade::market::SubscriptionManager& manager, FakeUpstream& upstream) {
  manager.on_connection_changed(true);
  static_cast<void>(manager.snapshot());
  const auto desired = upstream.registry.desired();
  upstream.registry.confirm({{"T", "subscription"},
                             {"trades", desired},
                             {"quotes", desired},
                             {"bars", desired},
                             {"updatedBars", desired}});
  manager.on_confirmation(desired);
  static_cast<void>(manager.snapshot());
}

const simtrade::market::SymbolSubscription* find_state(
    const std::vector<simtrade::market::SymbolSubscription>& states,
    const std::string& symbol) {
  const auto found = std::find_if(states.begin(), states.end(), [&](const auto& state) {
    return state.symbol == symbol;
  });
  return found == states.end() ? nullptr : &*found;
}

void test_asset_parsing_and_search() {
  const nlohmann::json assets = nlohmann::json::array({
      {{"symbol", "AAPL"}, {"name", "Apple Inc."}, {"exchange", "NASDAQ"}, {"class", "us_equity"},
       {"status", "active"}, {"tradable", true}, {"fractionable", true}},
      {{"symbol", "SPY"}, {"name", "SPDR S&P 500 ETF Trust"}, {"exchange", "ARCA"}, {"class", "us_equity"},
       {"status", "active"}, {"tradable", true}, {"fractionable", true}},
      {{"symbol", "OLD"}, {"name", "Inactive Corp"}, {"class", "us_equity"}, {"status", "inactive"}},
      {{"symbol", "BTC/USD"}, {"name", "Bitcoin"}, {"class", "crypto"}, {"status", "active"}},
  });
  const auto catalogue = simtrade::market::parse_alpaca_assets(assets);
  check(catalogue.size() == 2, "asset parser keeps active US equities and ETFs only");
  check(catalogue[0].symbol == "AAPL" && catalogue[0].fractionable, "asset flags are preserved");
  check(simtrade::market::search_instruments(catalogue, "apple", 1, 20).total == 1,
        "search is case-insensitive across company names");
}

void test_registry_confirmation_and_reconnect() {
  simtrade::market::SubscriptionRegistry registry(30);
  check(registry.request("aapl") == simtrade::market::SubscriptionResult::added,
        "first low-level subscription is accepted");
  check(registry.request("AAPL") == simtrade::market::SubscriptionResult::duplicate,
        "duplicate low-level subscription is rejected");
  registry.confirm({{"T", "subscription"}, {"trades", {"AAPL"}}, {"quotes", {"AAPL"}}});
  check(registry.confirmed() == std::set<std::string>{"AAPL"}, "subscription acknowledgement is tracked");
  registry.release("AAPL");
  check(registry.confirmed().contains("AAPL"), "released slot remains confirmed until Alpaca acknowledgement");
  registry.disconnected();
  check(registry.confirmed().empty(), "confirmed subscriptions clear on disconnect");
}

void test_pinned_and_dynamic_capacity() {
  FakeUpstream upstream;
  simtrade::market::SubscriptionManager manager(
      upstream, [](const std::string&) { return true; }, pinned_symbols(), 30, 0s, 0s);
  confirm_desired(manager, upstream);
  auto states = manager.snapshot();
  check(states.size() == 5, "exactly five configured symbols are initialized");
  check(std::all_of(states.begin(), states.end(), [](const auto& state) { return state.pinned; }),
        "all five configured symbols remain pinned");

  for (int index = 0; index < 25; ++index) {
    check(manager.watch(std::string("D") + std::to_string(index)).state == simtrade::market::WatchState::connecting,
          "each of the 25 dynamic slots is available");
  }
  check(manager.active_symbol_count() == 30, "five pinned and 25 dynamic symbols consume all 30 slots");
  const auto queued = manager.watch("D25");
  check(queued.state == simtrade::market::WatchState::queued && queued.queue_position == 1,
        "a 26th actively viewed dynamic symbol enters the waiting queue");
}

void test_viewer_reference_counting_and_disconnect_cleanup() {
  FakeUpstream upstream;
  simtrade::market::SubscriptionManager manager(
      upstream, [](const std::string&) { return true; }, pinned_symbols(), 30, 0s, 0s);
  manager.watch("SHARED");
  manager.watch("SHARED");
  auto states = manager.snapshot();
  const auto* shared = find_state(states, "SHARED");
  check(shared != nullptr && shared->viewer_count == 2, "multiple viewers increment one symbol reference count");
  check(upstream.registry.desired().size() == 6, "multiple viewers consume one Alpaca symbol slot");
  manager.unwatch("SHARED");
  manager.unwatch("SHARED");
  manager.unwatch("SHARED");
  shared = find_state(manager.snapshot(), "SHARED");
  check(shared != nullptr && shared->viewer_count == 0,
        "unwatch and disconnect-style cleanup never make viewer counts negative");

  FakeViewerSink viewer_sink;
  std::map<std::string, std::string> cache{{"market:quote:AAPL", nlohmann::json({{"type", "quote"}, {"symbol", "AAPL"}}).dump()}};
  simtrade::market::MarketDataHub hub(viewer_sink, [&cache](const std::string& key) -> std::optional<std::string> {
    const auto found = cache.find(key);
    return found == cache.end() ? std::nullopt : std::optional<std::string>(found->second);
  });
  std::vector<std::string> messages;
  const auto client = hub.add_client([&messages](const std::string& message) { messages.push_back(message); });
  hub.handle_client_message(client, R"({"action":"watch","symbol":"AAPL"})");
  hub.handle_client_message(client, R"({"action":"watch","symbol":"AAPL"})");
  check(viewer_sink.watches.size() == 1 && viewer_sink.touches == std::vector<std::string>{"AAPL"} &&
            hub.viewer_count("AAPL") == 1,
        "duplicate watch requests from one connection are idempotent");
  hub.remove_client(client);
  hub.remove_client(client);
  check(viewer_sink.unwatches == std::vector<std::string>{"AAPL"} && hub.viewer_count("AAPL") == 0,
        "disconnect cleanup removes every viewer exactly once");
}

void test_lru_grace_residency_and_protection() {
  FakeUpstream upstream;
  auto now = std::chrono::steady_clock::time_point(100s);
  simtrade::market::SubscriptionManager manager(
      upstream,
      [](const std::string&) { return true; },
      pinned_symbols(),
      30,
      30s,
      30s,
      [&now] { return now; });
  confirm_desired(manager, upstream);
  for (int index = 0; index < 25; ++index) {
    now += 1s;
    manager.watch(std::string("L") + std::to_string(index));
  }
  confirm_desired(manager, upstream);

  manager.unwatch("L0");
  manager.unwatch("L1");
  static_cast<void>(manager.snapshot());
  check(!manager.eviction_candidate(), "grace and minimum residency block immediate eviction");
  now += 31s;
  check(manager.eviction_candidate() == std::optional<std::string>("L0"),
        "LRU chooses the eligible symbol with the oldest lastRequestedAt");

  manager.watch("L0");
  check(manager.eviction_candidate() == std::optional<std::string>("L1"),
        "an actively viewed symbol cannot be evicted");
  manager.protect_order("L1");
  check(!manager.eviction_candidate(), "a pending-order symbol cannot be evicted");

  const auto result = manager.watch("REPLACE");
  check(result.state == simtrade::market::WatchState::queued,
        "protected and viewed symbols queue demand instead of evicting a user");
  const auto states = manager.snapshot();
  check(std::all_of(states.begin(), states.end(), [](const auto& state) {
          return !state.pinned || !state.unsubscribe_pending;
        }), "pinned symbols cannot be eviction victims");
}

void test_safe_lru_confirmation_sequence() {
  FakeUpstream upstream;
  auto now = std::chrono::steady_clock::time_point(100s);
  simtrade::market::SubscriptionManager manager(
      upstream, [](const std::string&) { return true; }, pinned_symbols(), 30, 0s, 0s, [&now] { return now; });
  confirm_desired(manager, upstream);
  for (int index = 0; index < 25; ++index) {
    now += 1s;
    manager.watch(std::string("E") + std::to_string(index));
  }
  confirm_desired(manager, upstream);
  manager.unwatch("E0");
  static_cast<void>(manager.snapshot());
  now += 1s;

  check(manager.watch("NEW").state == simtrade::market::WatchState::connecting,
        "eligible LRU starts a replacement");
  check(!upstream.registry.desired().contains("NEW") && upstream.registry.desired().size() == 29,
        "replacement is not subscribed before eviction confirmation");
  confirm_desired(manager, upstream);
  static_cast<void>(manager.snapshot());
  check(upstream.registry.desired().contains("NEW") && upstream.registry.desired().size() == 30,
        "released slot is reused only after unsubscribe confirmation");
  confirm_desired(manager, upstream);
  const auto* replacement = find_state(manager.snapshot(), "NEW");
  check(replacement != nullptr && replacement->subscribed && !replacement->subscribe_pending,
        "frontend symbol becomes live only after subscribe confirmation");
}

void test_pending_order_capacity_and_concurrent_serialization() {
  FakeUpstream upstream;
  simtrade::market::SubscriptionManager manager(
      upstream, [](const std::string&) { return true; }, pinned_symbols(), 30, 0s, 0s);
  manager.restore_pending_orders({{"O0", 3}});
  const auto* restored = find_state(manager.snapshot(), "O0");
  check(restored != nullptr && restored->pending_order_count == 3,
        "persisted non-terminal order counts rebuild symbol protection after restart");
  for (int index = 1; index < 25; ++index) {
    check(manager.protect_order(std::string("O") + std::to_string(index)).state ==
              simtrade::market::WatchState::connecting,
          "pending orders reserve dynamic symbols");
  }
  const auto protected_queue = manager.protect_order("O25");
  check(protected_queue.state == simtrade::market::WatchState::queued && protected_queue.queue_position == 1,
        "the 31st protected symbol is retained in the order-priority queue");

  FakeUpstream concurrent_upstream;
  simtrade::market::SubscriptionManager concurrent_manager(
      concurrent_upstream, [](const std::string&) { return true; }, pinned_symbols(), 30, 0s, 0s);
  std::vector<std::thread> threads;
  std::atomic<int> accepted{0};
  for (int index = 0; index < 80; ++index) {
    threads.emplace_back([&, index] {
      const auto result = concurrent_manager.watch(std::string("C") + std::to_string(index));
      if (result.state == simtrade::market::WatchState::connecting ||
          result.state == simtrade::market::WatchState::live) {
        ++accepted;
      }
    });
  }
  for (auto& thread : threads) thread.join();
  check(accepted == 25, "concurrent requests expose exactly 25 dynamic slots");
  check(concurrent_manager.active_symbol_count() == 30 && concurrent_upstream.registry.desired().size() == 30,
        "concurrent requests never exceed 30 confirmed or pending symbols");
  check(concurrent_upstream.maximum_parallel_calls == 1,
        "all upstream subscription changes pass through one serialized command loop");
}

void test_waiting_queue_deduplication_priority_and_abandonment() {
  FakeUpstream upstream;
  auto now = std::chrono::steady_clock::time_point(100s);
  simtrade::market::SubscriptionManager manager(
      upstream, [](const std::string&) { return true; }, pinned_symbols(), 30, 30s, 30s, [&now] { return now; });
  confirm_desired(manager, upstream);
  for (int index = 0; index < 25; ++index) {
    manager.protect_order(std::string("P") + std::to_string(index));
    now += 1s;
  }
  confirm_desired(manager, upstream);

  const auto oldest = manager.watch("WAIT1");
  now += 1s;
  const auto newer = manager.watch("WAIT2");
  const auto popular = manager.watch("WAIT2");
  check(oldest.state == simtrade::market::WatchState::queued && oldest.queue_position == 1,
        "first capacity request receives queue position one");
  check(newer.state == simtrade::market::WatchState::queued && popular.queue_position == 1,
        "larger viewer demand moves one deduplicated symbol ahead");
  check(manager.queued_symbol_count() == 2, "duplicate symbol demand shares a single queue entry");
  const auto queue = manager.queue_snapshot();
  check(queue.size() == 2 && queue[0].symbol == "WAIT2" && queue[0].viewer_demand == 2 &&
            queue[1].symbol == "WAIT1",
        "queue ordering uses viewer demand before age");

  const auto order_request = manager.protect_order("ORDERQ");
  check(order_request.state == simtrade::market::WatchState::queued && order_request.queue_position == 1,
        "valid order demand has highest queue priority");
  manager.release_order("ORDERQ");
  static_cast<void>(manager.snapshot());
  check(manager.queued_symbol_count() == 2, "empty order-only queue entries are deleted");

  manager.unwatch("WAIT2");
  manager.unwatch("WAIT2");
  static_cast<void>(manager.snapshot());
  check(manager.queued_symbol_count() == 1 && manager.queue_snapshot()[0].symbol == "WAIT1",
        "abandoned viewer demand is removed before allocation");
}

void test_queue_activation_waits_for_alpaca_confirmation() {
  FakeUpstream upstream;
  auto now = std::chrono::steady_clock::time_point(100s);
  simtrade::market::SubscriptionManager manager(
      upstream, [](const std::string&) { return true; }, pinned_symbols(), 30, 0s, 0s, [&now] { return now; });
  confirm_desired(manager, upstream);
  for (int index = 0; index < 25; ++index) {
    manager.watch(std::string("A") + std::to_string(index));
    now += 1s;
  }
  confirm_desired(manager, upstream);

  manager.watch("OLDER");
  now += 1s;
  manager.watch("POPULAR");
  manager.watch("POPULAR");
  manager.unwatch("A0");
  static_cast<void>(manager.snapshot());
  manager.process_queue();
  check(!upstream.registry.desired().contains("POPULAR") && upstream.registry.desired().size() == 29,
        "highest-priority queued symbol waits for unsubscribe confirmation");

  confirm_desired(manager, upstream);
  check(upstream.registry.desired().contains("POPULAR") && upstream.registry.desired().size() == 30,
        "freed slot activates the highest-priority queued symbol");
  check(!upstream.registry.desired().contains("OLDER"), "lower-priority queue entries remain queued");
  confirm_desired(manager, upstream);
  const auto* popular = find_state(manager.snapshot(), "POPULAR");
  check(popular != nullptr && popular->subscribed && manager.queued_symbol_count() == 1,
        "queued demand becomes live only after subscribe confirmation");
}

void test_authentication_normalization_and_routing() {
  const auto authentication = simtrade::market::authentication_message("key-id", "secret-value");
  check(authentication == nlohmann::json({{"action", "auth"}, {"key", "key-id"}, {"secret", "secret-value"}}),
        "Alpaca authentication message uses backend credentials");
  const auto events = simtrade::market::normalize_alpaca_events(
      nlohmann::json::array({{{"T", "q"}, {"S", "AAPL"}, {"bp", 225.10}, {"ap", 225.14},
                             {"t", "2026-09-23T10:15:30.123Z"}}}),
      "iex");
  check(events.size() == 1 && events[0].payload["type"] == "quote", "quotes remain normalized");

  FakeViewerSink sink;
  simtrade::market::MarketDataHub hub(sink, {});
  std::vector<std::string> first_messages;
  std::vector<std::string> second_messages;
  const auto first = hub.add_client([&](const std::string& message) { first_messages.push_back(message); });
  const auto second = hub.add_client([&](const std::string& message) { second_messages.push_back(message); });
  hub.handle_client_message(first, R"({"action":"watch","symbol":"AAPL"})");
  hub.handle_client_message(second, R"({"action":"watch","symbol":"MSFT"})");
  const auto before_first = first_messages.size();
  const auto before_second = second_messages.size();
  hub.publish({{"type", "trade"}, {"symbol", "AAPL"}, {"price", 225.2}});
  check(first_messages.size() == before_first + 1 && second_messages.size() == before_second,
        "frontend fan-out remains symbol-specific");
}

}  // namespace

int main() {
  test_asset_parsing_and_search();
  test_registry_confirmation_and_reconnect();
  test_pinned_and_dynamic_capacity();
  test_viewer_reference_counting_and_disconnect_cleanup();
  test_lru_grace_residency_and_protection();
  test_safe_lru_confirmation_sequence();
  test_pending_order_capacity_and_concurrent_serialization();
  test_waiting_queue_deduplication_priority_and_abandonment();
  test_queue_activation_waits_for_alpaca_confirmation();
  test_authentication_normalization_and_routing();
  if (failures == 0) {
    std::cout << "All market-data tests passed.\n";
    return EXIT_SUCCESS;
  }
  std::cerr << failures << " market-data test(s) failed.\n";
  return EXIT_FAILURE;
}
