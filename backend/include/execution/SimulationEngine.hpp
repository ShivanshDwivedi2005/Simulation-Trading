#pragma once

#include "market/MarketDataProvider.hpp"

#include <optional>
#include <string>

namespace simtrade::execution {

enum class Side { Buy, Sell };
enum class OrderType { Market, Limit, Stop, StopLimit };

struct OrderRequest {
  std::string order_id;
  std::string instrument_id;
  Side side;
  OrderType type;
  double quantity;
  std::optional<double> limit_price;
  std::optional<double> stop_price;
};

struct ExecutionResult {
  bool accepted;
  bool filled;
  double fill_price;
  std::string reason;
};

class SimulationEngine {
 public:
  [[nodiscard]] ExecutionResult execute(const OrderRequest& order, const market::Quote& quote, double available_buying_power) const;
};

}  // namespace simtrade::execution
