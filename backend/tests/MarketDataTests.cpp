#include "market/MarketDataCore.hpp"
#include "market/MarketDataHub.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& description) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << description << '\n';
}

class FakeSubscriptionSink final : public simtrade::market::SymbolSubscriptionSink {
 public:
  explicit FakeSubscriptionSink(std::size_t maximum = 30) : registry(maximum) {}

  simtrade::market::SubscriptionResult subscribe(const std::string& symbol) override {
    requests.push_back(symbol);
    return registry.request(symbol);
  }

  bool unsubscribe(const std::string& symbol) override {
    releases.push_back(symbol);
    return registry.release(symbol);
  }

  simtrade::market::SubscriptionRegistry registry;
  std::vector<std::string> requests;
  std::vector<std::string> releases;
};

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

  const auto apple = simtrade::market::search_instruments(catalogue, "apple", 1, 20);
  check(apple.total == 1 && apple.instruments[0].symbol == "AAPL", "search is case-insensitive across company names");
  const auto second_page = simtrade::market::search_instruments(catalogue, "", 2, 1);
  check(second_page.total == 2 && second_page.instruments.size() == 1 && second_page.instruments[0].symbol == "SPY",
        "instrument search paginates deterministically");
}

void test_subscriptions_and_reconnect_state() {
  simtrade::market::SubscriptionRegistry registry(30);
  check(registry.request("aapl") == simtrade::market::SubscriptionResult::added,
        "first subscription is accepted");
  check(registry.request("AAPL") == simtrade::market::SubscriptionResult::duplicate,
        "duplicate subscription is rejected");
  for (int index = 1; index < 30; ++index) {
    check(registry.request(std::string("S") + std::to_string(index)) == simtrade::market::SubscriptionResult::added,
          "subscription within the 30-symbol limit is accepted");
  }
  check(registry.request("OVER") == simtrade::market::SubscriptionResult::limit_reached,
        "the thirty-first distinct symbol is rejected");

  registry.confirm({{"T", "subscription"}, {"trades", {"AAPL"}}, {"quotes", {"AAPL"}},
                    {"bars", {"AAPL"}}, {"updatedBars", {"AAPL"}}});
  check(registry.confirmed() == std::set<std::string>{"AAPL"}, "subscription acknowledgement is tracked");
  registry.disconnected();
  check(registry.confirmed().empty(), "confirmed subscriptions clear on disconnect");
  check(registry.desired().size() == 30 && registry.desired().contains("AAPL"),
        "desired subscriptions survive disconnect for resubscription");
}

void test_authentication_and_normalization() {
  const auto authentication = simtrade::market::authentication_message("key-id", "secret-value");
  check(authentication == nlohmann::json({{"action", "auth"}, {"key", "key-id"}, {"secret", "secret-value"}}),
        "Alpaca authentication message uses backend credentials");

  const nlohmann::json messages = nlohmann::json::array({
      {{"T", "q"}, {"S", "AAPL"}, {"bp", 225.10}, {"bs", 2}, {"ap", 225.14}, {"as", 4},
       {"t", "2026-09-23T10:15:30.123Z"}},
      {{"T", "t"}, {"S", "AAPL"}, {"p", 225.12}, {"s", 3}, {"t", "2026-09-23T10:15:30.124Z"}},
      {{"T", "b"}, {"S", "AAPL"}, {"o", 224.0}, {"h", 226.0}, {"l", 223.5}, {"c", 225.0},
       {"v", 1000}, {"t", "2026-09-23T10:15:00Z"}},
  });
  const auto events = simtrade::market::normalize_alpaca_events(messages, "iex");
  check(events.size() == 3, "quotes, trades and bars are normalized");
  check(events[0].payload["type"] == "quote" && events[0].payload["source"] == "alpaca_iex" &&
            events[0].payload["bidPrice"] == 225.10,
        "quote normalization uses the public event contract");

  std::map<std::string, std::string> cache;
  simtrade::market::cache_normalized_event(events[0], [&cache](const std::string& key, const std::string& value) {
    cache[key] = value;
  });
  check(cache.contains("market:quote:AAPL"), "normalized quote updates the expected Redis cache key");
}

void test_frontend_routing() {
  FakeSubscriptionSink upstream;
  std::map<std::string, std::string> cache{{"market:quote:AAPL", nlohmann::json({{"type", "quote"}, {"symbol", "AAPL"}}).dump()}};
  simtrade::market::MarketDataHub hub(upstream, [&cache](const std::string& key) -> std::optional<std::string> {
    const auto value = cache.find(key);
    return value == cache.end() ? std::nullopt : std::optional<std::string>(value->second);
  });
  std::vector<std::string> first_messages;
  std::vector<std::string> second_messages;
  const auto first = hub.add_client([&first_messages](const std::string& message) { first_messages.push_back(message); });
  const auto second = hub.add_client([&second_messages](const std::string& message) { second_messages.push_back(message); });

  hub.handle_client_message(first, nlohmann::json({{"action", "subscribe"}, {"symbols", {"AAPL"}}}).dump());
  hub.handle_client_message(second, nlohmann::json({{"action", "subscribe"}, {"symbols", {"MSFT"}}}).dump());
  check(upstream.requests.size() == 2, "one upstream request is made for each distinct interested symbol");
  check(!first_messages.empty() && nlohmann::json::parse(first_messages.front())["type"] == "quote",
        "cached market data is sent immediately after subscription");

  const auto before_first = first_messages.size();
  const auto before_second = second_messages.size();
  hub.publish({{"type", "trade"}, {"symbol", "AAPL"}, {"price", 225.2}});
  check(first_messages.size() == before_first + 1, "interested frontend receives its symbol update");
  check(second_messages.size() == before_second, "uninterested frontend does not receive another symbol update");

  hub.handle_client_message(first, nlohmann::json({{"action", "subscribe"}, {"symbols", {"AAPL"}}}).dump());
  check(upstream.requests.size() == 2, "duplicate frontend subscription does not duplicate the upstream request");
  hub.remove_client(first);
  check(upstream.releases == std::vector<std::string>{"AAPL"}, "last interested client releases the upstream symbol");
  hub.remove_client(second);
}

}  // namespace

int main() {
  test_asset_parsing_and_search();
  test_subscriptions_and_reconnect_state();
  test_authentication_and_normalization();
  test_frontend_routing();
  if (failures == 0) {
    std::cout << "All market-data tests passed.\n";
    return EXIT_SUCCESS;
  }
  std::cerr << failures << " market-data test(s) failed.\n";
  return EXIT_FAILURE;
}
