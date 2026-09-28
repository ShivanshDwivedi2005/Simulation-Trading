#include "order/OrderRepository.hpp"

#include <pqxx/pqxx>

#include <chrono>
#include <limits>
#include <stdexcept>

namespace {

constexpr auto order_columns =
    "id, client_order_id, trader_id, instrument_numeric_id, side, order_type, status, "
    "limit_price_ticks, stop_price_ticks, quantity::bigint AS quantity, "
    "remaining_quantity::bigint AS remaining_quantity, sequence_number, version, rejection_reason, "
    "floor(extract(epoch FROM created_at) * 1000)::bigint AS created_at_ms, "
    "floor(extract(epoch FROM updated_at) * 1000)::bigint AS updated_at_ms, stop_activated";

std::string select_order_sql(std::string_view where_clause, std::string_view suffix = {}) {
  return std::string("SELECT ") + order_columns +
         " FROM (SELECT o.*, i.id AS instrument_numeric_id FROM orders o "
         "JOIN instruments i ON i.instrument_id = o.instrument_id) persisted_order WHERE " +
         std::string(where_clause) + std::string(suffix);
}

std::uint64_t as_unsigned(const pqxx::field& field) {
  const auto value = field.as<long long>();
  if (value <= 0) throw std::runtime_error("database identifier is outside the supported range");
  return static_cast<std::uint64_t>(value);
}

simtrade::order::Order map_order(const pqxx::row& row) {
  using namespace simtrade::order;
  const auto instrument = as_unsigned(row["instrument_numeric_id"]);
  const auto version = as_unsigned(row["version"]);
  if (instrument > std::numeric_limits<InstrumentId>::max() || version > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("database order value is outside the supported range");
  }
  Order order;
  order.id = as_unsigned(row["id"]);
  order.clientOrderId = row["client_order_id"].as<std::string>();
  order.traderId = as_unsigned(row["trader_id"]);
  order.instrumentId = static_cast<InstrumentId>(instrument);
  order.side = side_from_string(row["side"].as<std::string>());
  order.type = order_type_from_string(row["order_type"].as<std::string>());
  order.status = order_status_from_string(row["status"].as<std::string>());
  if (!row["limit_price_ticks"].is_null()) order.limitPrice = row["limit_price_ticks"].as<Price>();
  if (!row["stop_price_ticks"].is_null()) order.stopPrice = row["stop_price_ticks"].as<Price>();
  order.quantity = row["quantity"].as<Quantity>();
  order.remainingQuantity = row["remaining_quantity"].as<Quantity>();
  order.sequenceNumber = as_unsigned(row["sequence_number"]);
  order.version = static_cast<std::uint32_t>(version);
  if (!row["rejection_reason"].is_null()) order.rejectionReason = row["rejection_reason"].as<std::string>();
  order.createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(row["created_at_ms"].as<std::int64_t>()));
  order.updatedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(row["updated_at_ms"].as<std::int64_t>()));
  order.stopActivated = row["stop_activated"].as<bool>();
  validate(order);
  return order;
}

std::vector<simtrade::order::Order> map_orders(const pqxx::result& rows) {
  std::vector<simtrade::order::Order> orders;
  orders.reserve(rows.size());
  for (const auto& row : rows) orders.push_back(map_order(row));
  return orders;
}

}  // namespace

namespace simtrade::order {

OrderRepository::OrderRepository(const std::string& connection_string)
    : connection_(std::make_unique<pqxx::connection>(connection_string)) {
  if (!connection_->is_open()) throw std::runtime_error("database connection is not open");
  prepare_statements();
}

OrderRepository::~OrderRepository() = default;

void OrderRepository::prepare_statements() {
  connection_->prepare(
      "order_insert",
      "INSERT INTO orders (user_id, account_id, instrument_id, client_order_id, trader_id, side, order_type, "
      "status, quantity, remaining_quantity, limit_price_ticks, stop_price_ticks, limit_price, stop_price) "
      "SELECT u.user_id, a.account_id, i.instrument_id, $1, u.trader_id, $4, $5, $6, $9, $9, "
      "NULLIF($7, '')::bigint, NULLIF($8, '')::bigint, "
      "NULLIF($7, '')::bigint * i.tick_size, NULLIF($8, '')::bigint * i.tick_size "
      "FROM users u JOIN trading_accounts a ON a.user_id = u.user_id AND a.status = 'ACTIVE' "
      "JOIN instruments i ON i.id = $3 AND i.status = 'ACTIVE' "
      "WHERE u.trader_id = $2 ORDER BY a.created_at LIMIT 1 "
      "ON CONFLICT (client_order_id) DO NOTHING RETURNING id");
  connection_->prepare("order_find", select_order_sql("id = $1"));
  connection_->prepare("order_find_client", select_order_sql("client_order_id = $1"));
  connection_->prepare(
      "order_status_update",
      "UPDATE orders SET status = $3, rejection_reason = NULLIF($5, ''), version = version + 1, updated_at = now() "
      "WHERE id = $1 AND trader_id = $2 AND version = $4 "
      "AND status NOT IN ('FILLED', 'CANCELLED', 'REJECTED', 'EXPIRED') RETURNING id");
  connection_->prepare(
      "order_remaining_update",
      "UPDATE orders SET remaining_quantity = $3, "
      "status = CASE WHEN $3 = 0 THEN 'FILLED' WHEN $3 < quantity THEN 'PARTIALLY_FILLED' ELSE status END, "
      "filled_at = CASE WHEN $3 = 0 THEN now() ELSE filled_at END, version = version + 1, updated_at = now() "
      "WHERE id = $1 AND trader_id = $2 AND version = $4 AND $3 >= 0 AND $3 <= quantity "
      "AND status NOT IN ('FILLED', 'CANCELLED', 'REJECTED', 'EXPIRED') RETURNING id");
  connection_->prepare(
      "order_cancel",
      "UPDATE orders SET status = 'CANCELLED', cancelled_at = now(), version = version + 1, updated_at = now() "
      "WHERE id = $1 AND trader_id = $2 AND version = $3 "
      "AND status IN ('PENDING', 'OPEN', 'PARTIALLY_FILLED') RETURNING id");
  connection_->prepare(
      "order_stop_activate",
      "UPDATE orders SET stop_activated = true, version = version + 1, updated_at = now() "
      "WHERE id = $1 AND trader_id = $2 AND version = $3 AND stop_activated = false "
      "AND order_type IN ('STOP', 'STOP_LIMIT') "
      "AND status IN ('PENDING', 'OPEN', 'PARTIALLY_FILLED') RETURNING id");
  connection_->prepare(
      "execution_insert",
      "INSERT INTO executions (execution_id, order_id, trader_id, instrument_id, execution_price_ticks, "
      "execution_quantity, market_bid_ticks, market_ask_ticks, price_source, market_timestamp) "
      "VALUES ($1, $2, $3, $4, $5, $6, NULLIF($7, '')::bigint, NULLIF($8, '')::bigint, $9, $10::timestamptz) "
      "ON CONFLICT (execution_id) DO NOTHING RETURNING id");
  connection_->prepare(
      "execution_apply",
      "UPDATE orders SET remaining_quantity = remaining_quantity - $4, "
      "status = CASE WHEN remaining_quantity = $4 THEN 'FILLED' ELSE 'PARTIALLY_FILLED' END, "
      "filled_at = CASE WHEN remaining_quantity = $4 THEN now() ELSE filled_at END, "
      "version = version + 1, updated_at = now() "
      "WHERE id = $1 AND trader_id = $2 AND instrument_id = (SELECT instrument_id FROM instruments WHERE id = $3) "
      "AND version = $5 AND $4 > 0 AND remaining_quantity >= $4 "
      "AND status IN ('PENDING', 'OPEN', 'PARTIALLY_FILLED') RETURNING id");
  connection_->prepare(
      "event_insert",
      "INSERT INTO order_events (event_id, order_id, event_type, event_data) VALUES ($1, $2, $3, $4::jsonb) "
      "ON CONFLICT (event_id) DO NOTHING RETURNING id");
  connection_->prepare("orders_active", select_order_sql("status IN ('PENDING', 'OPEN', 'PARTIALLY_FILLED')", " ORDER BY sequence_number"));
  connection_->prepare("orders_active_instrument", select_order_sql("instrument_numeric_id = $1 AND status IN ('PENDING', 'OPEN', 'PARTIALLY_FILLED')", " ORDER BY sequence_number"));
  connection_->prepare("orders_trader_history", select_order_sql("trader_id = $1", " ORDER BY created_at DESC, id DESC LIMIT $2"));
  connection_->prepare(
      "instruments_active",
      "SELECT id, symbol, name, tick_size::text, active, matching_group FROM instruments "
      "WHERE status = 'ACTIVE' ORDER BY symbol");
}

Order OrderRepository::insert_order(const NewOrder& order) {
  if (order.clientOrderId.empty() || order.traderId == 0 || order.instrumentId == 0 || order.quantity <= 0) {
    throw std::invalid_argument("client order id, identifiers, and quantity must be valid");
  }
  if ((order.type == OrderType::Limit || order.type == OrderType::StopLimit) && (!order.limitPrice || *order.limitPrice <= 0)) {
    throw std::invalid_argument("limit price is required");
  }
  if ((order.type == OrderType::Stop || order.type == OrderType::StopLimit) && (!order.stopPrice || *order.stopPrice <= 0)) {
    throw std::invalid_argument("stop price is required");
  }

  std::scoped_lock lock(mutex_);
  pqxx::work transaction(*connection_);
  const std::string side(to_string(order.side));
  const std::string type(to_string(order.type));
  const std::string status(to_string(order.status));
  const auto inserted = transaction.exec_prepared(
      "order_insert", order.clientOrderId, order.traderId, order.instrumentId, side,
      type, status, order.limitPrice ? std::to_string(*order.limitPrice) : "",
      order.stopPrice ? std::to_string(*order.stopPrice) : "", order.quantity);
  const auto rows = transaction.exec_prepared("order_find_client", order.clientOrderId);
  if (rows.empty()) throw std::runtime_error("active trader or instrument not found");
  auto persisted = map_order(rows[0]);
  if (inserted.empty() && (persisted.traderId != order.traderId || persisted.instrumentId != order.instrumentId ||
                           persisted.side != order.side || persisted.type != order.type ||
                           persisted.quantity != order.quantity || persisted.limitPrice != order.limitPrice ||
                           persisted.stopPrice != order.stopPrice)) {
    throw std::runtime_error("client order id already belongs to a different order");
  }
  transaction.commit();
  return persisted;
}

std::optional<Order> OrderRepository::find_order(OrderId id) const {
  std::scoped_lock lock(mutex_);
  pqxx::read_transaction transaction(*connection_);
  const auto rows = transaction.exec_prepared("order_find", id);
  return rows.empty() ? std::nullopt : std::optional<Order>(map_order(rows[0]));
}

std::optional<Order> OrderRepository::find_order_by_client_order_id(const std::string& client_order_id) const {
  std::scoped_lock lock(mutex_);
  pqxx::read_transaction transaction(*connection_);
  const auto rows = transaction.exec_prepared("order_find_client", client_order_id);
  return rows.empty() ? std::nullopt : std::optional<Order>(map_order(rows[0]));
}

bool OrderRepository::update_status(OrderId id, TraderId trader_id, OrderStatus status,
                                    std::uint32_t expected_version,
                                    std::optional<std::string> rejection_reason) {
  std::scoped_lock lock(mutex_);
  pqxx::work transaction(*connection_);
  const auto rows = transaction.exec_prepared("order_status_update", id, trader_id, std::string(to_string(status)),
                                              expected_version, rejection_reason.value_or(""));
  transaction.commit();
  return !rows.empty();
}

bool OrderRepository::update_remaining_quantity(OrderId id, TraderId trader_id, Quantity remaining_quantity,
                                                std::uint32_t expected_version) {
  std::scoped_lock lock(mutex_);
  pqxx::work transaction(*connection_);
  const auto rows = transaction.exec_prepared("order_remaining_update", id, trader_id, remaining_quantity, expected_version);
  transaction.commit();
  return !rows.empty();
}

bool OrderRepository::cancel_order(OrderId id, TraderId trader_id, std::uint32_t expected_version) {
  std::scoped_lock lock(mutex_);
  pqxx::work transaction(*connection_);
  const auto rows = transaction.exec_prepared("order_cancel", id, trader_id, expected_version);
  transaction.commit();
  return !rows.empty();
}

bool OrderRepository::activate_stop(OrderId id, TraderId trader_id, std::uint32_t expected_version) {
  std::scoped_lock lock(mutex_);
  pqxx::work transaction(*connection_);
  const auto rows = transaction.exec_prepared("order_stop_activate", id, trader_id, expected_version);
  transaction.commit();
  return !rows.empty();
}

bool OrderRepository::record_execution(const Execution& execution, std::uint32_t expected_order_version) {
  if (execution.executionId.empty() || execution.orderId == 0 || execution.traderId == 0 ||
      execution.instrumentId == 0 || execution.executionPrice <= 0 || execution.executionQuantity <= 0 ||
      execution.priceSource.empty() || execution.marketTimestamp.empty()) {
    throw std::invalid_argument("execution fields are invalid");
  }
  std::scoped_lock lock(mutex_);
  pqxx::work transaction(*connection_);
  const auto inserted = transaction.exec_prepared(
      "execution_insert", execution.executionId, execution.orderId, execution.traderId, execution.instrumentId,
      execution.executionPrice, execution.executionQuantity,
      execution.marketBid ? std::to_string(*execution.marketBid) : "",
      execution.marketAsk ? std::to_string(*execution.marketAsk) : "", execution.priceSource,
      execution.marketTimestamp);
  if (inserted.empty()) {
    transaction.commit();
    return false;
  }
  const auto updated = transaction.exec_prepared("execution_apply", execution.orderId, execution.traderId,
                                                 execution.instrumentId, execution.executionQuantity,
                                                 expected_order_version);
  if (updated.empty()) throw std::runtime_error("execution conflicts with the current order version or quantity");
  transaction.exec_prepared("event_insert", "execution:" + execution.executionId, execution.orderId,
                            "EXECUTION_RECORDED", nlohmann::json({{"execution_id", execution.executionId}}).dump());
  transaction.commit();
  return true;
}

bool OrderRepository::record_order_event(const std::string& event_id, OrderId order_id,
                                         const std::string& event_type, const nlohmann::json& event_data) {
  std::scoped_lock lock(mutex_);
  pqxx::work transaction(*connection_);
  const auto rows = transaction.exec_prepared("event_insert", event_id, order_id, event_type, event_data.dump());
  transaction.commit();
  return !rows.empty();
}

std::vector<Order> OrderRepository::load_active_orders() const {
  std::scoped_lock lock(mutex_);
  pqxx::read_transaction transaction(*connection_);
  return map_orders(transaction.exec_prepared("orders_active"));
}

std::vector<Order> OrderRepository::load_active_orders_for_instrument(InstrumentId instrument_id) const {
  std::scoped_lock lock(mutex_);
  pqxx::read_transaction transaction(*connection_);
  return map_orders(transaction.exec_prepared("orders_active_instrument", instrument_id));
}

std::vector<Order> OrderRepository::load_trader_order_history(TraderId trader_id, std::size_t limit) const {
  std::scoped_lock lock(mutex_);
  pqxx::read_transaction transaction(*connection_);
  return map_orders(transaction.exec_prepared("orders_trader_history", trader_id, limit));
}

std::vector<Instrument> OrderRepository::load_active_instruments() const {
  std::scoped_lock lock(mutex_);
  pqxx::read_transaction transaction(*connection_);
  const auto rows = transaction.exec_prepared("instruments_active");
  std::vector<Instrument> instruments;
  instruments.reserve(rows.size());
  for (const auto& row : rows) {
    const auto id = as_unsigned(row["id"]);
    if (id > std::numeric_limits<InstrumentId>::max()) throw std::runtime_error("instrument id is outside the supported range");
    instruments.push_back({static_cast<InstrumentId>(id), row["symbol"].as<std::string>(),
                           row["name"].as<std::string>(), row["tick_size"].as<std::string>(),
                           row["active"].as<bool>(), static_cast<std::uint8_t>(row["matching_group"].as<int>())});
  }
  return instruments;
}

}  // namespace simtrade::order
