#pragma once

#include "order/Order.hpp"

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <pqxx/connection>
#include <string>
#include <vector>

namespace simtrade::order {

struct NewOrder {
  std::string clientOrderId;
  TraderId traderId{};
  InstrumentId instrumentId{};
  Side side{Side::Buy};
  OrderType type{OrderType::Market};
  OrderStatus status{OrderStatus::Pending};
  std::optional<Price> limitPrice;
  std::optional<Price> stopPrice;
  Quantity quantity{};
};

struct Execution {
  std::string executionId;
  OrderId orderId{};
  TraderId traderId{};
  InstrumentId instrumentId{};
  Price executionPrice{};
  Quantity executionQuantity{};
  std::optional<Price> marketBid;
  std::optional<Price> marketAsk;
  std::string priceSource;
  std::string marketTimestamp;
};

class OrderRepository {
 public:
  explicit OrderRepository(const std::string& connection_string);
  ~OrderRepository();
  OrderRepository(const OrderRepository&) = delete;
  OrderRepository& operator=(const OrderRepository&) = delete;

  [[nodiscard]] Order insert_order(const NewOrder& order);
  [[nodiscard]] std::optional<Order> find_order(OrderId id) const;
  [[nodiscard]] std::optional<Order> find_order_by_client_order_id(const std::string& client_order_id) const;
  [[nodiscard]] bool update_status(OrderId id, TraderId trader_id, OrderStatus status,
                                   std::uint32_t expected_version,
                                   std::optional<std::string> rejection_reason = std::nullopt);
  [[nodiscard]] bool update_remaining_quantity(OrderId id, TraderId trader_id, Quantity remaining_quantity,
                                               std::uint32_t expected_version);
  [[nodiscard]] bool cancel_order(OrderId id, TraderId trader_id, std::uint32_t expected_version);
  [[nodiscard]] bool activate_stop(OrderId id, TraderId trader_id, std::uint32_t expected_version);
  [[nodiscard]] bool record_execution(const Execution& execution, std::uint32_t expected_order_version);
  [[nodiscard]] bool record_order_event(const std::string& event_id, OrderId order_id,
                                        const std::string& event_type, const nlohmann::json& event_data);
  [[nodiscard]] std::vector<Order> load_active_orders() const;
  [[nodiscard]] std::vector<Order> load_active_orders_for_instrument(InstrumentId instrument_id) const;
  [[nodiscard]] std::vector<Order> load_trader_order_history(TraderId trader_id, std::size_t limit = 100) const;
  [[nodiscard]] std::vector<Instrument> load_active_instruments() const;

 private:
  void prepare_statements();

  std::unique_ptr<pqxx::connection> connection_;
  mutable std::mutex mutex_;
};

}  // namespace simtrade::order
