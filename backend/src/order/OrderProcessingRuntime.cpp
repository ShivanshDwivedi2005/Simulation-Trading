#include "order/OrderProcessingRuntime.hpp"

#include "order/RedisOrderStore.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <iostream>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace {

std::int64_t now_milliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const auto position = static_cast<std::size_t>(
      std::ceil(fraction * static_cast<double>(values.size())) - 1.0);
  return values[std::min(position, values.size() - 1)];
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
                                   const simtrade::order::Instrument& instrument) {
  if (!event.contains(field) || !event[field].is_number()) throw std::invalid_argument("market price is missing");
  return simtrade::order::price_to_ticks(decimal_string(event[field].get<double>()), instrument.tickSize);
}

simtrade::order::Quantity event_size(const nlohmann::json& event, const char* field) {
  if (!event.contains(field) || !event[field].is_number()) return 0;
  const auto value = event[field].get<double>();
  if (!std::isfinite(value) || value <= 0.0) return 0;
  return static_cast<simtrade::order::Quantity>(std::floor(value));
}

}  // namespace

namespace simtrade::order {

InstrumentRouter::InstrumentRouter(std::vector<Instrument> instruments) {
  for (auto& instrument : instruments) {
    if (!instrument.active) continue;
    if (instrument.id == 0 || instrument.symbol.empty()) {
      throw std::invalid_argument("active instruments require an id and symbol");
    }
    if (instrument.matchingGroup != 1 && instrument.matchingGroup != 2) {
      throw std::invalid_argument("active instrument matching group must be 1 or 2");
    }
    if (!groups_by_id_.emplace(instrument.id, instrument.matchingGroup).second ||
        !ids_by_symbol_.emplace(instrument.symbol, instrument.id).second) {
      throw std::invalid_argument("active instrument ids and symbols must be unique");
    }
    groups_[instrument.matchingGroup - 1].push_back(std::move(instrument));
  }
  if (groups_[0].size() != 15 || groups_[1].size() != 15) {
    throw std::invalid_argument("each matching group must own exactly 15 active instruments");
  }
}

std::uint8_t InstrumentRouter::group_for(InstrumentId instrument_id) const {
  const auto found = groups_by_id_.find(instrument_id);
  return found == groups_by_id_.end() ? 0 : found->second;
}

std::uint8_t InstrumentRouter::group_for_symbol(const std::string& symbol) const {
  const auto id = instrument_for_symbol(symbol);
  return id == 0 ? 0 : group_for(id);
}

InstrumentId InstrumentRouter::instrument_for_symbol(const std::string& symbol) const {
  const auto found = ids_by_symbol_.find(symbol);
  return found == ids_by_symbol_.end() ? 0 : found->second;
}

const Instrument* InstrumentRouter::instrument(InstrumentId instrument_id) const {
  const auto matching_group = group_for(instrument_id);
  if (matching_group == 0) return nullptr;
  const auto& owned = groups_[matching_group - 1];
  const auto found = std::find_if(owned.begin(), owned.end(), [instrument_id](const Instrument& value) {
    return value.id == instrument_id;
  });
  return found == owned.end() ? nullptr : &*found;
}

const std::vector<Instrument>& InstrumentRouter::group(std::uint8_t matching_group) const {
  if (matching_group != 1 && matching_group != 2) throw std::out_of_range("matching group must be 1 or 2");
  return groups_[matching_group - 1];
}

std::size_t InstrumentRouter::group_size(std::uint8_t matching_group) const {
  return group(matching_group).size();
}

class OrderProcessingRuntime::Worker {
 public:
  using ResultHandler = std::function<void(const MatchingCommand&, const MatchingResult&, bool)>;
  using DuplicateHandler = std::function<void()>;

  Worker(std::string redis_url, std::uint32_t maximum_price_age_ms,
         std::vector<Instrument> instruments, std::size_t matching_batch_size,
         concurrency::BoundedQueue<MatchingCommand>& queue, ResultHandler result_handler,
         DuplicateHandler duplicate_handler, std::atomic<std::size_t>& active_commands,
         std::string identifier_namespace)
      : store_(std::move(redis_url), maximum_price_age_ms),
        engine_(store_, std::move(instruments), matching_batch_size, {}, {},
                std::move(identifier_namespace)), queue_(queue),
        result_handler_(std::move(result_handler)), duplicate_handler_(std::move(duplicate_handler)),
        active_commands_(active_commands) {}

  void start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
  }

  void join() {
    if (thread_.joinable()) thread_.join();
  }

  [[nodiscard]] bool healthy() const noexcept { return healthy_.load(); }

 private:
  bool remember(const std::string& command_id) {
    if (command_id.empty()) return true;
    if (!command_ids_.insert(command_id).second) return false;
    command_order_.push_back(command_id);
    if (command_order_.size() > 100000) {
      command_ids_.erase(command_order_.front());
      command_order_.pop_front();
    }
    return true;
  }

  void run() {
    healthy_ = true;
    MatchingCommand command;
    while (queue_.pop(command)) {
      active_commands_.fetch_add(1);
      if (!remember(command.commandId)) {
        duplicate_handler_();
        result_handler_(command, {}, true);
        active_commands_.fetch_sub(1);
        continue;
      }
      MatchingResult result;
      bool succeeded = false;
      try {
        std::visit(
            [this, &result](const auto& payload) {
              using Payload = std::decay_t<decltype(payload)>;
              if constexpr (std::is_same_v<Payload, nlohmann::json>) {
                result = engine_.process(payload);
              } else if constexpr (std::is_same_v<Payload, AddOrderCommand>) {
                if (!store_.add_order(payload.order)) throw std::runtime_error("active order was not added");
              } else if constexpr (std::is_same_v<Payload, CancelOrderCommand>) {
                if (!store_.cancel_order(payload.orderId, payload.traderId, payload.expectedVersion,
                                         payload.eventId)) {
                  throw std::runtime_error("active order was not cancelled");
                }
              } else if constexpr (std::is_same_v<Payload, ModifyQuantityCommand>) {
                if (!store_.modify_quantity(payload.orderId, payload.traderId, payload.quantity,
                                            payload.sequenceNumber, payload.expectedVersion,
                                            payload.eventId)) {
                  throw std::runtime_error("active order quantity was not changed");
                }
              } else if constexpr (std::is_same_v<Payload, ModifyPriceCommand>) {
                if (!store_.modify_price(payload.orderId, payload.traderId, payload.limitPrice,
                                         payload.stopPrice, payload.sequenceNumber,
                                         payload.expectedVersion, payload.eventId)) {
                  throw std::runtime_error("active order price was not changed");
                }
              }
            },
            command.payload);
        succeeded = true;
      } catch (const std::exception& exception) {
        healthy_ = false;
        std::cerr << nlohmann::json({{"level", "error"},
                                     {"event", "matching_command_failed"},
                                     {"instrument_id", command.instrumentId},
                                     {"command_id", command.commandId},
                                     {"message", exception.what()}}).dump() << std::endl;
      }
      result_handler_(command, result, succeeded);
      active_commands_.fetch_sub(1);
      healthy_ = true;
    }
    healthy_ = false;
    running_ = false;
  }

  RedisOrderStore store_;
  MatchingEngine engine_;
  concurrency::BoundedQueue<MatchingCommand>& queue_;
  ResultHandler result_handler_;
  DuplicateHandler duplicate_handler_;
  std::atomic<std::size_t>& active_commands_;
  std::unordered_set<std::string> command_ids_;
  std::deque<std::string> command_order_;
  std::atomic<bool> running_{false};
  std::atomic<bool> healthy_{false};
  std::thread thread_;
};

OrderProcessingRuntime::OrderProcessingRuntime(std::string redis_url, std::vector<Instrument> instruments,
                                               std::uint32_t maximum_price_age_ms,
                                               std::size_t matching_batch_size,
                                               std::size_t queue_capacity)
    : router_(std::move(instruments)), market_store_(redis_url, maximum_price_age_ms),
      queue1_(queue_capacity), queue2_(queue_capacity) {
  auto result_handler = [this](const MatchingCommand& command, const MatchingResult& result, bool succeeded) {
    record_result(command, result, succeeded);
  };
  auto duplicate_handler = [this] { duplicate_commands_.fetch_add(1); };
  worker1_ = std::make_unique<Worker>(redis_url, maximum_price_age_ms, router_.group(1),
                                      matching_batch_size, queue1_, result_handler, duplicate_handler,
                                      active_commands_, "matching-1");
  worker2_ = std::make_unique<Worker>(std::move(redis_url), maximum_price_age_ms, router_.group(2),
                                      matching_batch_size, queue2_, result_handler, duplicate_handler,
                                      active_commands_, "matching-2");
}

OrderProcessingRuntime::~OrderProcessingRuntime() { stop(); }

void OrderProcessingRuntime::start() {
  if (accepting_.exchange(true)) return;
  worker1_->start();
  worker2_->start();
}

void OrderProcessingRuntime::stop_accepting() { accepting_ = false; }

void OrderProcessingRuntime::stop() {
  accepting_ = false;
  queue1_.close();
  queue2_.close();
  worker1_->join();
  worker2_->join();
}

bool OrderProcessingRuntime::wait_until_idle(std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (queue1_.size() == 0 && queue2_.size() == 0 && active_commands_.load() == 0 &&
        commands_processed_.load() >= enqueued_commands_.load()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return queue1_.size() == 0 && queue2_.size() == 0 && active_commands_.load() == 0 &&
         commands_processed_.load() >= enqueued_commands_.load();
}

EnqueueResult OrderProcessingRuntime::route_market(nlohmann::json event, std::chrono::milliseconds timeout) {
  const auto symbol = event.value("symbol", "");
  const auto instrument_id = router_.instrument_for_symbol(symbol);
  const auto* instrument = router_.instrument(instrument_id);
  const auto type = event.value("type", "");
  const auto timestamp = event.value("timestamp", "");
  const auto market_time = timestamp_milliseconds(timestamp);
  const auto received_time = now_milliseconds();
  if (instrument == nullptr || (type != "quote" && type != "trade") || market_time <= 0 ||
      market_time - received_time > 1000 ||
      received_time - market_time > static_cast<std::int64_t>(market_store_.maximum_price_age_ms())) {
    return EnqueueResult::InvalidInstrument;
  }
  if (type == "quote") {
    MarketSnapshot snapshot{instrument_id, event_price(event, "bidPrice", *instrument),
                            event_price(event, "askPrice", *instrument), event_size(event, "bidSize"),
                            event_size(event, "askSize"), 0, event.value("source", "unknown"),
                            timestamp, market_time, received_time};
    if (const auto trade = market_store_.latest_trade(instrument_id)) snapshot.lastTradePrice = trade->lastTradePrice;
    market_store_.store_quote(snapshot);
  } else {
    MarketSnapshot snapshot{instrument_id, 0, 0, 0, 0, event_price(event, "price", *instrument),
                            event.value("source", "unknown"), timestamp, market_time, received_time};
    if (const auto quote = market_store_.latest_quote(instrument_id)) {
      snapshot.bidPrice = quote->bidPrice;
      snapshot.askPrice = quote->askPrice;
      snapshot.bidSize = quote->bidSize;
      snapshot.askSize = quote->askSize;
    }
    market_store_.store_trade(snapshot);
  }
  const auto command_id = "market:" + std::to_string(instrument_id) + ':' +
                          type + ':' + timestamp;
  return enqueue({instrument_id, command_id, std::move(event), std::chrono::steady_clock::now()}, timeout);
}

EnqueueResult OrderProcessingRuntime::route_order(const Order& order, const std::string& command_id,
                                                  std::chrono::milliseconds timeout) {
  return enqueue({order.instrumentId, command_id, AddOrderCommand{order}, std::chrono::steady_clock::now()}, timeout);
}

EnqueueResult OrderProcessingRuntime::route_cancel(InstrumentId instrument_id, CancelOrderCommand command,
                                                   const std::string& command_id,
                                                   std::chrono::milliseconds timeout) {
  return enqueue({instrument_id, command_id, std::move(command), std::chrono::steady_clock::now()}, timeout);
}

EnqueueResult OrderProcessingRuntime::route_quantity_change(InstrumentId instrument_id,
                                                            ModifyQuantityCommand command,
                                                            const std::string& command_id,
                                                            std::chrono::milliseconds timeout) {
  return enqueue({instrument_id, command_id, std::move(command), std::chrono::steady_clock::now()}, timeout);
}

EnqueueResult OrderProcessingRuntime::route_price_change(InstrumentId instrument_id,
                                                         ModifyPriceCommand command,
                                                         const std::string& command_id,
                                                         std::chrono::milliseconds timeout) {
  return enqueue({instrument_id, command_id, std::move(command), std::chrono::steady_clock::now()}, timeout);
}

EnqueueResult OrderProcessingRuntime::enqueue(MatchingCommand command, std::chrono::milliseconds timeout) {
  if (!accepting_) return EnqueueResult::Stopped;
  const auto group = router_.group_for(command.instrumentId);
  if (group == 0) return EnqueueResult::InvalidInstrument;
  auto& queue = group == 1 ? queue1_ : queue2_;
  if (!queue.push(std::move(command), timeout)) return accepting_ ? EnqueueResult::Overloaded : EnqueueResult::Stopped;
  enqueued_commands_.fetch_add(1);
  return EnqueueResult::Accepted;
}

void OrderProcessingRuntime::record_result(const MatchingCommand& command, const MatchingResult& result,
                                           bool succeeded) {
  commands_processed_.fetch_add(1);
  if (std::holds_alternative<nlohmann::json>(command.payload)) {
    market_events_processed_.fetch_add(1);
    last_market_event_ms_ = now_milliseconds();
  } else {
    orders_processed_.fetch_add(1);
  }
  if (result.fills > 0) {
    fills_processed_.fetch_add(result.fills);
    last_successful_fill_ms_ = now_milliseconds();
  }
  if (!succeeded) failures_.fetch_add(1);
  const auto latency = std::chrono::duration<double, std::micro>(
      std::chrono::steady_clock::now() - command.enqueuedAt).count();
  {
    std::scoped_lock lock(latency_mutex_);
    latency_samples_micros_.push_back(latency);
    if (latency_samples_micros_.size() > 100000) {
      latency_samples_micros_.erase(latency_samples_micros_.begin(), latency_samples_micros_.begin() + 1000);
    }
  }
}

ProcessingHealth OrderProcessingRuntime::health() const {
  ProcessingHealth value;
  value.accepting = accepting_;
  value.worker1Healthy = worker1_->healthy();
  value.worker2Healthy = worker2_->healthy();
  value.matchingQueue1Size = queue1_.size();
  value.matchingQueue2Size = queue2_.size();
  value.matchingQueue1MaximumDepth = queue1_.maximum_depth();
  value.matchingQueue2MaximumDepth = queue2_.maximum_depth();
  value.lastMarketEventMs = last_market_event_ms_;
  value.lastSuccessfulFillMs = last_successful_fill_ms_;
  value.metrics.commandsProcessed = commands_processed_;
  value.metrics.ordersProcessed = orders_processed_;
  value.metrics.marketEventsProcessed = market_events_processed_;
  value.metrics.fillsProcessed = fills_processed_;
  value.metrics.duplicateCommands = duplicate_commands_;
  value.metrics.failures = failures_;
  value.metrics.maximumQueueDepth = std::max(queue1_.maximum_depth(), queue2_.maximum_depth());
  std::vector<double> samples;
  {
    std::scoped_lock lock(latency_mutex_);
    samples = latency_samples_micros_;
  }
  if (!samples.empty()) {
    double sum = 0.0;
    for (const auto sample : samples) sum += sample;
    value.metrics.averageLatencyMicros = sum / static_cast<double>(samples.size());
    value.metrics.p95LatencyMicros = percentile(samples, 0.95);
    value.metrics.p99LatencyMicros = percentile(samples, 0.99);
  }
  return value;
}

const InstrumentRouter& OrderProcessingRuntime::router() const noexcept { return router_; }

std::size_t OrderProcessingRuntime::matching_worker_count() const noexcept { return 2; }

}  // namespace simtrade::order
