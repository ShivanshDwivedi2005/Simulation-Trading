#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace simtrade::order {

using OrderId = std::uint64_t;
using TraderId = std::uint64_t;
using InstrumentId = std::uint32_t;
using Price = std::int64_t;
using Quantity = std::int64_t;

enum class Side { Buy, Sell };
enum class OrderType { Market, Limit, Stop, StopLimit };
enum class OrderStatus { Pending, Open, PartiallyFilled, Filled, Cancelled, Rejected, Expired };

struct Order {
  OrderId id{};
  std::string clientOrderId;
  TraderId traderId{};
  InstrumentId instrumentId{};
  Side side{Side::Buy};
  OrderType type{OrderType::Market};
  OrderStatus status{OrderStatus::Pending};
  std::optional<Price> limitPrice;
  std::optional<Price> stopPrice;
  Quantity quantity{};
  Quantity remainingQuantity{};
  std::uint64_t sequenceNumber{};
  std::uint32_t version{};
  std::optional<std::string> rejectionReason;
  std::chrono::system_clock::time_point createdAt;
  std::chrono::system_clock::time_point updatedAt;
};

struct Instrument {
  InstrumentId id{};
  std::string symbol;
  std::string name;
  std::string tickSize;
  bool active{};
  std::uint8_t matchingGroup{};
};

[[nodiscard]] std::string_view to_string(Side side) noexcept;
[[nodiscard]] std::string_view to_string(OrderType type) noexcept;
[[nodiscard]] std::string_view to_string(OrderStatus status) noexcept;
[[nodiscard]] Side side_from_string(std::string_view value);
[[nodiscard]] OrderType order_type_from_string(std::string_view value);
[[nodiscard]] OrderStatus order_status_from_string(std::string_view value);
[[nodiscard]] bool is_terminal(OrderStatus status) noexcept;
[[nodiscard]] Price price_to_ticks(std::string_view price, std::string_view tick_size);
[[nodiscard]] std::string ticks_to_price(Price ticks, std::string_view tick_size);
void validate(const Order& order);

}  // namespace simtrade::order
