#include "order/Order.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace {

struct Decimal {
  std::int64_t units;
  std::int64_t scale;
};

Decimal parse_decimal(std::string_view value) {
  if (value.empty()) throw std::invalid_argument("empty decimal value");
  bool negative = false;
  if (value.front() == '-' || value.front() == '+') {
    negative = value.front() == '-';
    value.remove_prefix(1);
  }
  if (value.empty()) throw std::invalid_argument("invalid decimal value");

  const auto point = value.find('.');
  const auto whole = point == std::string_view::npos ? value : value.substr(0, point);
  auto fraction = point == std::string_view::npos ? std::string_view{} : value.substr(point + 1);
  if (whole.empty() || fraction.size() > 9 || fraction.find('.') != std::string_view::npos) {
    throw std::invalid_argument("invalid decimal value");
  }

  std::int64_t scale = 1;
  for (std::size_t index = 0; index < fraction.size(); ++index) scale *= 10;
  std::int64_t whole_units = 0;
  const auto whole_result = std::from_chars(whole.data(), whole.data() + whole.size(), whole_units);
  if (whole_result.ec != std::errc{} || whole_result.ptr != whole.data() + whole.size()) {
    throw std::invalid_argument("invalid decimal value");
  }
  std::int64_t fractional_units = 0;
  if (!fraction.empty()) {
    const auto fraction_result = std::from_chars(fraction.data(), fraction.data() + fraction.size(), fractional_units);
    if (fraction_result.ec != std::errc{} || fraction_result.ptr != fraction.data() + fraction.size()) {
      throw std::invalid_argument("invalid decimal value");
    }
  }
  if (whole_units > (std::numeric_limits<std::int64_t>::max() - fractional_units) / scale) {
    throw std::out_of_range("decimal value is too large");
  }
  const auto units = whole_units * scale + fractional_units;
  return {negative ? -units : units, scale};
}

std::int64_t checked_multiply(std::int64_t left, std::int64_t right) {
  if (left != 0 && (left > std::numeric_limits<std::int64_t>::max() / right ||
                    left < std::numeric_limits<std::int64_t>::min() / right)) {
    throw std::out_of_range("price is too large");
  }
  return left * right;
}

}  // namespace

namespace simtrade::order {

std::string_view to_string(Side side) noexcept {
  return side == Side::Buy ? "BUY" : "SELL";
}

std::string_view to_string(OrderType type) noexcept {
  switch (type) {
    case OrderType::Market: return "MARKET";
    case OrderType::Limit: return "LIMIT";
    case OrderType::Stop: return "STOP";
    case OrderType::StopLimit: return "STOP_LIMIT";
  }
  return "MARKET";
}

std::string_view to_string(OrderStatus status) noexcept {
  switch (status) {
    case OrderStatus::Pending: return "PENDING";
    case OrderStatus::Open: return "OPEN";
    case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
    case OrderStatus::Filled: return "FILLED";
    case OrderStatus::Cancelled: return "CANCELLED";
    case OrderStatus::Rejected: return "REJECTED";
    case OrderStatus::Expired: return "EXPIRED";
  }
  return "REJECTED";
}

Side side_from_string(std::string_view value) {
  if (value == "BUY") return Side::Buy;
  if (value == "SELL") return Side::Sell;
  throw std::invalid_argument("invalid order side");
}

OrderType order_type_from_string(std::string_view value) {
  if (value == "MARKET") return OrderType::Market;
  if (value == "LIMIT") return OrderType::Limit;
  if (value == "STOP") return OrderType::Stop;
  if (value == "STOP_LIMIT") return OrderType::StopLimit;
  throw std::invalid_argument("invalid order type");
}

OrderStatus order_status_from_string(std::string_view value) {
  if (value == "PENDING") return OrderStatus::Pending;
  if (value == "OPEN") return OrderStatus::Open;
  if (value == "PARTIALLY_FILLED") return OrderStatus::PartiallyFilled;
  if (value == "FILLED") return OrderStatus::Filled;
  if (value == "CANCELLED") return OrderStatus::Cancelled;
  if (value == "REJECTED") return OrderStatus::Rejected;
  if (value == "EXPIRED") return OrderStatus::Expired;
  throw std::invalid_argument("invalid order status");
}

bool is_terminal(OrderStatus status) noexcept {
  return status == OrderStatus::Filled || status == OrderStatus::Cancelled ||
         status == OrderStatus::Rejected || status == OrderStatus::Expired;
}

Price price_to_ticks(std::string_view price, std::string_view tick_size) {
  const auto parsed_price = parse_decimal(price);
  const auto parsed_tick = parse_decimal(tick_size);
  if (parsed_price.units <= 0 || parsed_tick.units <= 0) throw std::invalid_argument("price and tick size must be positive");
  const auto numerator = checked_multiply(parsed_price.units, parsed_tick.scale);
  const auto denominator = checked_multiply(parsed_tick.units, parsed_price.scale);
  if (numerator % denominator != 0) throw std::invalid_argument("price is not aligned to the instrument tick size");
  return numerator / denominator;
}

std::string ticks_to_price(Price ticks, std::string_view tick_size) {
  const auto tick = parse_decimal(tick_size);
  if (ticks < 0 || tick.units <= 0) throw std::invalid_argument("ticks and tick size must be non-negative");
  const auto units = checked_multiply(ticks, tick.units);
  const auto whole = units / tick.scale;
  auto fraction = units % tick.scale;
  if (tick.scale == 1) return std::to_string(whole);
  auto digits = std::to_string(tick.scale).size() - 1;
  std::string result = std::to_string(whole) + "." + std::string(digits, '0');
  const auto fractional = std::to_string(fraction);
  std::copy(fractional.begin(), fractional.end(), result.end() - static_cast<std::ptrdiff_t>(fractional.size()));
  while (result.size() > 1 && result.back() == '0') result.pop_back();
  if (result.back() == '.') result.pop_back();
  return result;
}

void validate(const Order& order) {
  if (order.id == 0 || order.traderId == 0 || order.instrumentId == 0) throw std::invalid_argument("order identifiers must be positive");
  if (order.clientOrderId.empty()) throw std::invalid_argument("client order id is required");
  if (order.quantity <= 0) throw std::invalid_argument("quantity must be positive");
  if (order.remainingQuantity < 0 || order.remainingQuantity > order.quantity) throw std::invalid_argument("remaining quantity is invalid");
  if ((order.type == OrderType::Limit || order.type == OrderType::StopLimit) && (!order.limitPrice || *order.limitPrice <= 0)) {
    throw std::invalid_argument("limit price is required");
  }
  if ((order.type == OrderType::Stop || order.type == OrderType::StopLimit) && (!order.stopPrice || *order.stopPrice <= 0)) {
    throw std::invalid_argument("stop price is required");
  }
  if (order.stopActivated && order.type != OrderType::Stop && order.type != OrderType::StopLimit) {
    throw std::invalid_argument("only stop orders can be activated");
  }
}

}  // namespace simtrade::order
