#pragma once

#include "order/OrderRepository.hpp"
#include "order/RedisOrderStore.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace simtrade::order {

struct MatchingResult {
  std::size_t batches{};
  std::size_t maximumBatchSize{};
  std::size_t fills{};
  std::size_t activations{};
  std::size_t staleRejections{};
};

class MatchingEngine {
 public:
  using ExecutionHandler = std::function<void(const Execution&, std::uint32_t)>;
  using ActivationHandler = std::function<void(const Order&, std::uint32_t)>;

  MatchingEngine(RedisOrderStore& store, std::vector<Instrument> instruments,
                 std::size_t batch_size, ExecutionHandler execution_handler = {},
                 ActivationHandler activation_handler = {}, std::string identifier_namespace = {});
  [[nodiscard]] MatchingResult process(const nlohmann::json& event);
  [[nodiscard]] RecoveryResult recover(const std::vector<Order>& active_orders);

 private:
  [[nodiscard]] MatchingResult process_quote(const nlohmann::json& event, const Instrument& instrument);
  [[nodiscard]] MatchingResult process_trade(const nlohmann::json& event, const Instrument& instrument);
  void match_market(InstrumentId instrument_id, Side side, MarketSnapshot snapshot,
                    Quantity& liquidity, MatchingResult& result);
  void match_limits(InstrumentId instrument_id, Side side, Price market_price,
                    MarketSnapshot snapshot, Quantity& liquidity, MatchingResult& result);
  void activate_stops(InstrumentId instrument_id, Side side, MarketSnapshot snapshot,
                      MatchingResult& result);
  [[nodiscard]] bool fill(const Order& order, Quantity quantity, const MarketSnapshot& snapshot,
                          MatchingResult& result);
  [[nodiscard]] const Instrument* instrument_for_symbol(const std::string& symbol) const;
  [[nodiscard]] std::string next_identifier(const std::string& prefix);

  RedisOrderStore& store_;
  std::vector<Instrument> instruments_;
  std::map<std::string, std::size_t> instrument_indexes_;
  std::size_t batch_size_;
  ExecutionHandler execution_handler_;
  ActivationHandler activation_handler_;
  std::string identifier_namespace_;
  std::atomic<std::uint64_t> identifier_sequence_{1};
};

}  // namespace simtrade::order
