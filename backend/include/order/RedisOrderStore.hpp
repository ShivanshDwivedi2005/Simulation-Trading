#pragma once

#include "order/Order.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct redisContext;

namespace simtrade::order {

struct MarketSnapshot {
  InstrumentId instrumentId{};
  Price bidPrice{};
  Price askPrice{};
  Quantity bidSize{};
  Quantity askSize{};
  Price lastTradePrice{};
  std::string source;
  std::string marketTimestamp;
  std::int64_t marketTimestampMs{};
  std::int64_t receivedTimestampMs{};
};

struct FillRequest {
  std::string eventId;
  std::string executionId;
  OrderId orderId{};
  std::uint32_t expectedVersion{};
  Quantity quantity{};
  MarketSnapshot market;
  std::int64_t executionTimestampMs{};
};

struct FillResult {
  bool applied{};
  bool duplicate{};
  bool stale{};
  bool priceConditionFailed{};
  Quantity filledQuantity{};
  Quantity remainingQuantity{};
  Price executionPrice{};
  std::uint32_t version{};
  OrderStatus status{OrderStatus::Open};
};

struct RecoveryResult {
  std::size_t rebuilt{};
  std::size_t removed{};
  std::size_t invalid{};
};

class RedisOrderStore {
 public:
  RedisOrderStore(std::string redis_url, std::uint32_t maximum_price_age_ms);
  ~RedisOrderStore();
  RedisOrderStore(const RedisOrderStore&) = delete;
  RedisOrderStore& operator=(const RedisOrderStore&) = delete;

  [[nodiscard]] bool healthy();
  [[nodiscard]] std::string status() const;
  void reset_connection();

  [[nodiscard]] bool add_order(const Order& order);
  [[nodiscard]] bool cancel_order(OrderId order_id, TraderId trader_id, std::uint32_t expected_version,
                                  const std::string& event_id);
  [[nodiscard]] bool modify_quantity(OrderId order_id, TraderId trader_id, Quantity new_quantity,
                                     std::uint64_t new_sequence_number, std::uint32_t expected_version,
                                     const std::string& event_id);
  [[nodiscard]] bool modify_price(OrderId order_id, TraderId trader_id,
                                  std::optional<Price> limit_price, std::optional<Price> stop_price,
                                  std::uint64_t new_sequence_number, std::uint32_t expected_version,
                                  const std::string& event_id);
  [[nodiscard]] bool activate_stop(OrderId order_id, std::uint32_t expected_version,
                                   Price last_trade_price, std::int64_t market_timestamp_ms,
                                   std::int64_t now_ms, const std::string& event_id);
  [[nodiscard]] FillResult apply_fill(const FillRequest& request);

  void store_quote(const MarketSnapshot& snapshot);
  void store_trade(const MarketSnapshot& snapshot);
  [[nodiscard]] std::optional<MarketSnapshot> latest_quote(InstrumentId instrument_id);
  [[nodiscard]] std::optional<MarketSnapshot> latest_trade(InstrumentId instrument_id);
  [[nodiscard]] std::optional<Order> find_order(OrderId order_id);
  [[nodiscard]] std::vector<OrderId> eligible_limit_orders(InstrumentId instrument_id, Side side,
                                                           Price market_price, std::size_t limit);
  [[nodiscard]] std::vector<OrderId> eligible_stop_orders(InstrumentId instrument_id, Side side,
                                                          Price last_trade_price, std::size_t limit);
  [[nodiscard]] std::vector<OrderId> market_orders(InstrumentId instrument_id, Side side,
                                                   std::size_t limit);
  [[nodiscard]] std::vector<OrderId> indexed_orders(const std::string& key, std::size_t limit = 1000);
  [[nodiscard]] std::size_t stream_length();
  [[nodiscard]] std::uint32_t maximum_price_age_ms() const noexcept;
  [[nodiscard]] RecoveryResult recover(const std::vector<Order>& active_orders,
                                       const std::vector<Instrument>& active_instruments);

  [[nodiscard]] static std::string book_member(std::uint64_t sequence_number, OrderId order_id);
  [[nodiscard]] static std::string limit_book_key(InstrumentId instrument_id, Side side);
  [[nodiscard]] static std::string stop_book_key(InstrumentId instrument_id, Side side);
  [[nodiscard]] static std::string market_book_key(InstrumentId instrument_id, Side side);

 private:
  struct Reply;

  [[nodiscard]] std::unique_ptr<Reply> command(const std::vector<std::string>& arguments);
  [[nodiscard]] std::vector<std::unique_ptr<Reply>> pipeline(
      const std::vector<std::vector<std::string>>& commands);
  [[nodiscard]] bool connect_locked();
  void disconnect_locked();
  [[nodiscard]] std::vector<std::string> add_order_command(const Order& order) const;
  [[nodiscard]] bool remove_order_unchecked(OrderId order_id);

  std::string redis_url_;
  std::string host_;
  std::string username_;
  std::string password_;
  int port_{6379};
  int database_{0};
  std::uint32_t maximum_price_age_ms_;
  redisContext* context_{nullptr};
  mutable std::mutex mutex_;
  std::string error_;
};

}  // namespace simtrade::order
