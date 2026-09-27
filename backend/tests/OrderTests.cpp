#include "order/Order.hpp"
#include "order/OrderRepository.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

std::string unique_value(std::string_view prefix) {
  return std::string(prefix) + std::to_string(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

void test_model_and_price_conversion() {
  using namespace simtrade::order;
  check(to_string(side_from_string("BUY")) == "BUY", "side conversion round trips");
  check(to_string(order_type_from_string("STOP_LIMIT")) == "STOP_LIMIT", "order type conversion round trips");
  check(to_string(order_status_from_string("PARTIALLY_FILLED")) == "PARTIALLY_FILLED",
        "order status conversion round trips");
  check(is_terminal(OrderStatus::Filled) && is_terminal(OrderStatus::Expired) && !is_terminal(OrderStatus::Open),
        "terminal order statuses are classified correctly");
  check(price_to_ticks("123.45", "0.01") == 12345, "decimal prices convert to exact integer ticks");
  check(price_to_ticks("1.25", "0.05") == 25, "non-cent tick sizes convert exactly");
  check(ticks_to_price(12345, "0.01") == "123.45", "integer ticks convert back to decimal prices");
  bool rejected = false;
  try {
    static_cast<void>(price_to_ticks("1.23", "0.05"));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "prices not aligned to the tick size are rejected");
}

struct Fixture {
  simtrade::order::TraderId trader_id;
  simtrade::order::InstrumentId instrument_id;
};

Fixture create_fixture(const std::string& connection_string) {
  pqxx::connection connection(connection_string);
  pqxx::work transaction(connection);
  const auto email = unique_value("orders-") + "@example.test";
  const auto user = transaction.exec_params(
      "INSERT INTO users (name, email, email_verified_at, status) VALUES ('Order Test', $1, now(), 'ACTIVE') "
      "RETURNING user_id::text, trader_id",
      email);
  transaction.exec_params("INSERT INTO trading_accounts (user_id) VALUES ($1::uuid)",
                          user[0]["user_id"].as<std::string>());
  const auto instrument = transaction.exec(
      "SELECT id FROM instruments WHERE symbol = 'AAPL' AND status = 'ACTIVE'");
  transaction.commit();
  if (instrument.empty()) throw std::runtime_error("AAPL test instrument is missing");
  return {static_cast<simtrade::order::TraderId>(user[0]["trader_id"].as<long long>()),
          static_cast<simtrade::order::InstrumentId>(instrument[0]["id"].as<long long>())};
}

void test_database_constraints(const std::string& connection_string, const Fixture& fixture) {
  pqxx::connection connection(connection_string);
  pqxx::work transaction(connection);
  bool rejected = false;
  try {
    transaction.exec_params(
        "INSERT INTO orders (user_id, account_id, instrument_id, client_order_id, trader_id, side, order_type, "
        "status, quantity, remaining_quantity) "
        "SELECT u.user_id, a.account_id, i.instrument_id, $3, $1, 'BUY', 'LIMIT', 'OPEN', 1, 1 "
        "FROM users u JOIN trading_accounts a ON a.user_id = u.user_id "
        "JOIN instruments i ON i.id = $2 WHERE u.trader_id = $1",
        fixture.trader_id, fixture.instrument_id, unique_value("invalid-limit-"));
  } catch (const pqxx::sql_error&) {
    rejected = true;
  }
  check(rejected, "database rejects a limit order without limit price ticks");
}

void test_repository(const std::string& connection_string, const Fixture& fixture) {
  using namespace simtrade::order;
  OrderRepository repository(connection_string);
  const auto first_client_id = unique_value("client-");
  NewOrder request{first_client_id, fixture.trader_id, fixture.instrument_id, Side::Buy,
                   OrderType::Limit, OrderStatus::Open, 15000, std::nullopt, 10};
  const auto inserted = repository.insert_order(request);
  check(inserted.id > 0 && inserted.sequenceNumber > 0 && inserted.version == 1,
        "successful insertion assigns unique identifiers and a sequence");
  check(inserted.limitPrice == 15000 && inserted.remainingQuantity == 10,
        "inserted order preserves tick price and quantity");

  const auto duplicate = repository.insert_order(request);
  check(duplicate.id == inserted.id, "duplicate client order ids return the original order");
  check(repository.find_order(inserted.id)->clientOrderId == first_client_id,
        "orders can be retrieved by numeric id");
  check(repository.find_order_by_client_order_id(first_client_id)->id == inserted.id,
        "orders can be retrieved by client order id");

  check(repository.update_status(inserted.id, fixture.trader_id, OrderStatus::Open, inserted.version),
        "status updates succeed at the expected version");
  check(!repository.update_status(inserted.id, fixture.trader_id, OrderStatus::Open, inserted.version),
        "stale optimistic versions are rejected");
  auto current = repository.find_order(inserted.id).value();
  check(repository.update_remaining_quantity(current.id, current.traderId, 6, current.version),
        "remaining quantity updates are persisted");
  current = repository.find_order(inserted.id).value();
  check(current.status == OrderStatus::PartiallyFilled && current.remainingQuantity == 6,
        "partial fills update order status");
  check(repository.cancel_order(current.id, current.traderId, current.version),
        "an owned non-terminal order can be cancelled");
  current = repository.find_order(inserted.id).value();
  check(current.status == OrderStatus::Cancelled && !repository.cancel_order(current.id, current.traderId, current.version),
        "cancelled orders are terminal and retained");

  const auto execution_order = repository.insert_order(
      {unique_value("execution-order-"), fixture.trader_id, fixture.instrument_id, Side::Buy,
       OrderType::Limit, OrderStatus::Open, 15100, std::nullopt, 5});
  const auto execution_id = unique_value("execution-");
  Execution execution{execution_id, execution_order.id, fixture.trader_id, fixture.instrument_id,
                      15095, 2, 15090, 15100, "test", "2026-09-28T10:00:00Z"};
  check(repository.record_execution(execution, execution_order.version), "a new execution is recorded atomically");
  check(!repository.record_execution(execution, execution_order.version), "duplicate execution ids are idempotent");
  const auto executed = repository.find_order(execution_order.id).value();
  check(executed.remainingQuantity == 3 && executed.status == OrderStatus::PartiallyFilled,
        "execution persistence updates the order in the same transaction");

  check(repository.record_order_event(unique_value("event-"), execution_order.id, "TEST_EVENT", {{"ok", true}}),
        "order events are persisted");
  const auto history = repository.load_trader_order_history(fixture.trader_id);
  check(history.size() >= 2 && history.front().createdAt >= history.back().createdAt,
        "trader history is returned newest first");
  const auto active = repository.load_active_orders_for_instrument(fixture.instrument_id);
  check(std::any_of(active.begin(), active.end(), [&](const auto& order) { return order.id == execution_order.id; }),
        "active orders can be loaded for one instrument");

  OrderRepository restarted(connection_string);
  const auto restored = restarted.load_active_orders();
  check(std::any_of(restored.begin(), restored.end(), [&](const auto& order) { return order.id == execution_order.id; }),
        "active orders reload after repository restart");
  const auto instruments = restarted.load_active_instruments();
  check(instruments.size() == 30, "the active instrument configuration contains 30 stocks");
  check(std::all_of(instruments.begin(), instruments.end(), [](const auto& instrument) {
          return instrument.id > 0 && instrument.active && (instrument.matchingGroup == 1 || instrument.matchingGroup == 2);
        }), "active instruments have numeric ids and valid matching groups");

  pqxx::connection connection(connection_string);
  pqxx::work transaction(connection);
  bool rejected_group = false;
  try {
    transaction.exec("UPDATE instruments SET matching_group = 3 WHERE symbol = 'AAPL'");
  } catch (const pqxx::sql_error&) {
    rejected_group = true;
  }
  check(rejected_group, "database rejects matching groups outside one and two");
}

}  // namespace

int main() {
  test_model_and_price_conversion();
  const auto* database_url = std::getenv("SIMTRADE_TEST_DATABASE_URL");
  if (database_url == nullptr || std::string(database_url).empty()) {
    std::cout << "Database tests skipped because SIMTRADE_TEST_DATABASE_URL is not set.\n";
  } else {
    try {
      const auto fixture = create_fixture(database_url);
      test_database_constraints(database_url, fixture);
      test_repository(database_url, fixture);
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL: database test setup failed: " << exception.what() << '\n';
    }
  }
  if (failures == 0) {
    std::cout << "All order tests passed.\n";
    return EXIT_SUCCESS;
  }
  std::cerr << failures << " order test(s) failed.\n";
  return EXIT_FAILURE;
}
