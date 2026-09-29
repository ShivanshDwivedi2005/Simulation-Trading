#include "order/MatchingEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {

std::int64_t now_milliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t timestamp_milliseconds(const std::string& value) {
  if (value.size() < 19) return 0;
  std::tm parsed{};
  std::istringstream input(value.substr(0, 19));
  input >> std::get_time(&parsed, "%Y-%m-%dT%H:%M:%S");
  if (input.fail()) return 0;
  auto milliseconds = static_cast<std::int64_t>(timegm(&parsed)) * 1000;
  const auto point = value.find('.', 19);
  if (point != std::string::npos) {
    std::string fraction;
    for (auto index = point + 1; index < value.size() && std::isdigit(static_cast<unsigned char>(value[index])); ++index) {
      fraction.push_back(value[index]);
    }
    while (fraction.size() < 3) fraction.push_back('0');
    if (fraction.size() > 3) fraction.resize(3);
    if (!fraction.empty()) milliseconds += std::stoll(fraction);
  }
  return milliseconds;
}

std::string decimal_string(double value) {
  if (!std::isfinite(value) || value <= 0.0) throw std::invalid_argument("market price must be positive");
  std::ostringstream output;
  output << std::fixed << std::setprecision(9) << value;
  return output.str();
}

simtrade::order::Price event_price(const nlohmann::json& event, const char* field,
                                   const simtrade::order::Instrument& instrument, bool required) {
  if (!event.contains(field) || !event[field].is_number()) {
    if (required) throw std::invalid_argument(std::string("market event is missing ") + field);
    return 0;
  }
  const auto value = event[field].get<double>();
  if (value <= 0.0) {
    if (required) throw std::invalid_argument(std::string("market event has invalid ") + field);
    return 0;
  }
  return simtrade::order::price_to_ticks(decimal_string(value), instrument.tickSize);
}

simtrade::order::Quantity event_size(const nlohmann::json& event, const char* field) {
  if (!event.contains(field) || !event[field].is_number()) return 0;
  const auto value = event[field].get<double>();
  if (!std::isfinite(value) || value <= 0.0) return 0;
  return static_cast<simtrade::order::Quantity>(std::floor(value));
}

}  // namespace

namespace simtrade::order {

MatchingEngine::MatchingEngine(RedisOrderStore& store, std::vector<Instrument> instruments,
                               std::size_t batch_size, ExecutionHandler execution_handler,
                               ActivationHandler activation_handler, std::string identifier_namespace)
    : store_(store), instruments_(std::move(instruments)), batch_size_(batch_size),
      execution_handler_(std::move(execution_handler)), activation_handler_(std::move(activation_handler)),
      identifier_namespace_(std::move(identifier_namespace)) {
  if (batch_size_ == 0 || batch_size_ > 1000) throw std::invalid_argument("matching batch size must be between 1 and 1000");
  for (std::size_t index = 0; index < instruments_.size(); ++index) {
    if (instruments_[index].active) instrument_indexes_.emplace(instruments_[index].symbol, index);
  }
}

const Instrument* MatchingEngine::instrument_for_symbol(const std::string& symbol) const {
  const auto found = instrument_indexes_.find(symbol);
  return found == instrument_indexes_.end() ? nullptr : &instruments_[found->second];
}

std::string MatchingEngine::next_identifier(const std::string& prefix) {
  return prefix + (identifier_namespace_.empty() ? std::string{} : ':' + identifier_namespace_) + ':' +
         std::to_string(now_milliseconds()) + ':' +
         std::to_string(identifier_sequence_.fetch_add(1));
}

MatchingResult MatchingEngine::process(const nlohmann::json& event) {
  if (!event.is_object()) return {};
  const auto* instrument = instrument_for_symbol(event.value("symbol", ""));
  if (instrument == nullptr) return {};
  const auto type = event.value("type", "");
  if (type == "quote") return process_quote(event, *instrument);
  if (type == "trade") return process_trade(event, *instrument);
  return {};
}

MatchingResult MatchingEngine::process_quote(const nlohmann::json& event, const Instrument& instrument) {
  const auto received = now_milliseconds();
  const auto timestamp = event.value("timestamp", "");
  MarketSnapshot snapshot{
      instrument.id,
      event_price(event, "bidPrice", instrument, true),
      event_price(event, "askPrice", instrument, true),
      event_size(event, "bidSize"),
      event_size(event, "askSize"),
      0,
      event.value("source", "unknown"),
      timestamp,
      timestamp_milliseconds(timestamp),
      received,
  };
  if (const auto trade = store_.latest_trade(instrument.id)) snapshot.lastTradePrice = trade->lastTradePrice;
  store_.store_quote(snapshot);

  MatchingResult result;
  auto buy_liquidity = snapshot.askSize;
  auto sell_liquidity = snapshot.bidSize;
  match_market(instrument.id, Side::Buy, snapshot, buy_liquidity, result);
  match_limits(instrument.id, Side::Buy, snapshot.askPrice, snapshot, buy_liquidity, result);
  match_market(instrument.id, Side::Sell, snapshot, sell_liquidity, result);
  match_limits(instrument.id, Side::Sell, snapshot.bidPrice, snapshot, sell_liquidity, result);
  return result;
}

MatchingResult MatchingEngine::process_trade(const nlohmann::json& event, const Instrument& instrument) {
  const auto received = now_milliseconds();
  const auto timestamp = event.value("timestamp", "");
  MarketSnapshot snapshot{
      instrument.id,
      0,
      0,
      0,
      0,
      event_price(event, "price", instrument, true),
      event.value("source", "unknown"),
      timestamp,
      timestamp_milliseconds(timestamp),
      received,
  };
  auto execution_snapshot = snapshot;
  if (const auto quote = store_.latest_quote(instrument.id)) {
    execution_snapshot = *quote;
    execution_snapshot.lastTradePrice = snapshot.lastTradePrice;
    snapshot.bidPrice = quote->bidPrice;
    snapshot.askPrice = quote->askPrice;
    snapshot.bidSize = quote->bidSize;
    snapshot.askSize = quote->askSize;
  }
  store_.store_trade(snapshot);

  MatchingResult result;
  activate_stops(instrument.id, Side::Buy, snapshot, result);
  activate_stops(instrument.id, Side::Sell, snapshot, result);
  if (execution_snapshot.bidPrice > 0 && execution_snapshot.askPrice > 0) {
    auto buy_liquidity = execution_snapshot.askSize;
    auto sell_liquidity = execution_snapshot.bidSize;
    match_market(instrument.id, Side::Buy, execution_snapshot, buy_liquidity, result);
    match_limits(instrument.id, Side::Buy, execution_snapshot.askPrice, execution_snapshot, buy_liquidity, result);
    match_market(instrument.id, Side::Sell, execution_snapshot, sell_liquidity, result);
    match_limits(instrument.id, Side::Sell, execution_snapshot.bidPrice, execution_snapshot, sell_liquidity, result);
  }
  return result;
}

void MatchingEngine::match_market(InstrumentId instrument_id, Side side, MarketSnapshot snapshot,
                                  Quantity& liquidity, MatchingResult& result) {
  while (liquidity > 0) {
    const auto candidates = store_.market_orders(instrument_id, side, batch_size_);
    if (candidates.empty()) break;
    ++result.batches;
    result.maximumBatchSize = std::max(result.maximumBatchSize, candidates.size());
    bool progressed = false;
    for (const auto order_id : candidates) {
      if (liquidity <= 0) break;
      const auto order = store_.find_order(order_id);
      if (!order) continue;
      const auto quantity = std::min(order->remainingQuantity, liquidity);
      if (fill(*order, quantity, snapshot, result)) {
        liquidity -= quantity;
        progressed = true;
      }
    }
    if (!progressed || candidates.size() < batch_size_) break;
  }
}

void MatchingEngine::match_limits(InstrumentId instrument_id, Side side, Price market_price,
                                  MarketSnapshot snapshot, Quantity& liquidity, MatchingResult& result) {
  while (liquidity > 0) {
    const auto candidates = store_.eligible_limit_orders(instrument_id, side, market_price, batch_size_);
    if (candidates.empty()) break;
    ++result.batches;
    result.maximumBatchSize = std::max(result.maximumBatchSize, candidates.size());
    bool progressed = false;
    for (const auto order_id : candidates) {
      if (liquidity <= 0) break;
      const auto order = store_.find_order(order_id);
      if (!order) continue;
      const auto quantity = std::min(order->remainingQuantity, liquidity);
      if (fill(*order, quantity, snapshot, result)) {
        liquidity -= quantity;
        progressed = true;
      }
    }
    if (!progressed || candidates.size() < batch_size_) break;
  }
}

void MatchingEngine::activate_stops(InstrumentId instrument_id, Side side, MarketSnapshot snapshot,
                                    MatchingResult& result) {
  while (true) {
    const auto candidates = store_.eligible_stop_orders(instrument_id, side, snapshot.lastTradePrice, batch_size_);
    if (candidates.empty()) break;
    ++result.batches;
    result.maximumBatchSize = std::max(result.maximumBatchSize, candidates.size());
    bool progressed = false;
    for (const auto order_id : candidates) {
      const auto order = store_.find_order(order_id);
      if (!order) continue;
      const auto activated = store_.activate_stop(order_id, order->version, snapshot.lastTradePrice,
                                                  snapshot.marketTimestampMs, now_milliseconds(),
                                                  next_identifier("event"));
      if (activated) {
        if (activation_handler_) activation_handler_(*order, order->version);
        ++result.activations;
        progressed = true;
      }
    }
    if (!progressed || candidates.size() < batch_size_) break;
  }
}

bool MatchingEngine::fill(const Order& order, Quantity quantity, const MarketSnapshot& snapshot,
                          MatchingResult& result) {
  const auto execution_timestamp = now_milliseconds();
  const auto event_id = next_identifier("event");
  const auto execution_id = next_identifier("execution");
  const auto fill = store_.apply_fill({event_id, execution_id, order.id, order.version, quantity,
                                       snapshot, execution_timestamp});
  if (fill.stale) ++result.staleRejections;
  if (!fill.applied) return false;
  ++result.fills;
  if (execution_handler_) {
    execution_handler_(Execution{execution_id, order.id, order.traderId, order.instrumentId,
                                 fill.executionPrice, fill.filledQuantity,
                                 snapshot.bidPrice > 0 ? std::optional<Price>(snapshot.bidPrice) : std::nullopt,
                                 snapshot.askPrice > 0 ? std::optional<Price>(snapshot.askPrice) : std::nullopt,
                                 snapshot.source, snapshot.marketTimestamp}, order.version);
  }
  return true;
}

RecoveryResult MatchingEngine::recover(const std::vector<Order>& active_orders) {
  return store_.recover(active_orders, instruments_);
}

}  // namespace simtrade::order
