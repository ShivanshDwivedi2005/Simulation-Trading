#include "execution/SimulationEngine.hpp"

#include <cmath>

namespace simtrade::execution {

ExecutionResult SimulationEngine::execute(const OrderRequest& order, const market::Quote& quote, double available_buying_power) const {
  if (!std::isfinite(order.quantity) || order.quantity <= 0.0) return {false, false, 0.0, "quantity must be positive"};
  if (quote.bid <= 0.0 || quote.ask <= 0.0 || quote.ask < quote.bid) return {false, false, 0.0, "fresh valid quote unavailable"};
  if ((order.type == OrderType::Limit || order.type == OrderType::StopLimit) && (!order.limit_price || *order.limit_price <= 0.0)) return {false, false, 0.0, "valid limit price required"};
  if ((order.type == OrderType::Stop || order.type == OrderType::StopLimit) && (!order.stop_price || *order.stop_price <= 0.0)) return {false, false, 0.0, "valid stop price required"};

  const double market_price = order.side == Side::Buy ? quote.ask : quote.bid;
  if (order.side == Side::Buy && market_price * order.quantity > available_buying_power) return {false, false, 0.0, "insufficient buying power"};
  if (order.type == OrderType::Market) return {true, true, market_price, "filled from normalized quote"};
  return {true, false, 0.0, "accepted and awaiting trigger"};
}

}  // namespace simtrade::execution
