#pragma once

#include "concurrency/BoundedQueue.hpp"
#include "order/MatchingEngine.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace simtrade::order {

struct AddOrderCommand {
  Order order;
};

struct CancelOrderCommand {
  OrderId orderId{};
  TraderId traderId{};
  std::uint32_t expectedVersion{};
  std::string eventId;
};

struct ModifyQuantityCommand {
  OrderId orderId{};
  TraderId traderId{};
  Quantity quantity{};
  std::uint64_t sequenceNumber{};
  std::uint32_t expectedVersion{};
  std::string eventId;
};

struct ModifyPriceCommand {
  OrderId orderId{};
  TraderId traderId{};
  std::optional<Price> limitPrice;
  std::optional<Price> stopPrice;
  std::uint64_t sequenceNumber{};
  std::uint32_t expectedVersion{};
  std::string eventId;
};

using MatchingPayload = std::variant<nlohmann::json, AddOrderCommand, CancelOrderCommand,
                                     ModifyQuantityCommand, ModifyPriceCommand>;

struct MatchingCommand {
  InstrumentId instrumentId{};
  std::string commandId;
  MatchingPayload payload;
  std::chrono::steady_clock::time_point enqueuedAt{std::chrono::steady_clock::now()};
};

enum class EnqueueResult { Accepted, InvalidInstrument, Overloaded, Stopped };

struct ProcessingMetrics {
  std::uint64_t commandsProcessed{};
  std::uint64_t ordersProcessed{};
  std::uint64_t marketEventsProcessed{};
  std::uint64_t fillsProcessed{};
  std::uint64_t duplicateCommands{};
  std::uint64_t failures{};
  std::size_t maximumQueueDepth{};
  double averageLatencyMicros{};
  double p95LatencyMicros{};
  double p99LatencyMicros{};
};

struct ProcessingHealth {
  bool accepting{};
  bool worker1Healthy{};
  bool worker2Healthy{};
  std::size_t matchingQueue1Size{};
  std::size_t matchingQueue2Size{};
  std::size_t matchingQueue1MaximumDepth{};
  std::size_t matchingQueue2MaximumDepth{};
  std::int64_t lastMarketEventMs{};
  std::int64_t lastSuccessfulFillMs{};
  ProcessingMetrics metrics;
};

class InstrumentRouter {
 public:
  explicit InstrumentRouter(std::vector<Instrument> instruments);
  [[nodiscard]] std::uint8_t group_for(InstrumentId instrument_id) const;
  [[nodiscard]] std::uint8_t group_for_symbol(const std::string& symbol) const;
  [[nodiscard]] InstrumentId instrument_for_symbol(const std::string& symbol) const;
  [[nodiscard]] const Instrument* instrument(InstrumentId instrument_id) const;
  [[nodiscard]] const std::vector<Instrument>& group(std::uint8_t matching_group) const;
  [[nodiscard]] std::size_t group_size(std::uint8_t matching_group) const;

 private:
  std::map<InstrumentId, std::uint8_t> groups_by_id_;
  std::map<std::string, InstrumentId> ids_by_symbol_;
  std::array<std::vector<Instrument>, 2> groups_;
};

class OrderProcessingRuntime {
 public:
  OrderProcessingRuntime(std::string redis_url, std::vector<Instrument> instruments,
                         std::uint32_t maximum_price_age_ms, std::size_t matching_batch_size,
                         std::size_t queue_capacity);
  ~OrderProcessingRuntime();
  OrderProcessingRuntime(const OrderProcessingRuntime&) = delete;
  OrderProcessingRuntime& operator=(const OrderProcessingRuntime&) = delete;

  void start();
  void stop_accepting();
  void stop();
  [[nodiscard]] bool wait_until_idle(std::chrono::milliseconds timeout) const;

  [[nodiscard]] EnqueueResult route_market(nlohmann::json event, std::chrono::milliseconds timeout);
  [[nodiscard]] EnqueueResult route_order(const Order& order, const std::string& command_id,
                                          std::chrono::milliseconds timeout);
  [[nodiscard]] EnqueueResult route_cancel(InstrumentId instrument_id, CancelOrderCommand command,
                                           const std::string& command_id, std::chrono::milliseconds timeout);
  [[nodiscard]] EnqueueResult route_quantity_change(InstrumentId instrument_id, ModifyQuantityCommand command,
                                                    const std::string& command_id,
                                                    std::chrono::milliseconds timeout);
  [[nodiscard]] EnqueueResult route_price_change(InstrumentId instrument_id, ModifyPriceCommand command,
                                                 const std::string& command_id,
                                                 std::chrono::milliseconds timeout);

  [[nodiscard]] ProcessingHealth health() const;
  [[nodiscard]] const InstrumentRouter& router() const noexcept;
  [[nodiscard]] std::size_t matching_worker_count() const noexcept;

 private:
  class Worker;

  [[nodiscard]] EnqueueResult enqueue(MatchingCommand command, std::chrono::milliseconds timeout);
  void record_result(const MatchingCommand& command, const MatchingResult& result, bool succeeded);

  InstrumentRouter router_;
  RedisOrderStore market_store_;
  concurrency::BoundedQueue<MatchingCommand> queue1_;
  concurrency::BoundedQueue<MatchingCommand> queue2_;
  std::unique_ptr<Worker> worker1_;
  std::unique_ptr<Worker> worker2_;
  std::atomic<bool> accepting_{false};
  std::atomic<std::size_t> active_commands_{0};
  std::atomic<std::uint64_t> enqueued_commands_{0};
  std::atomic<std::uint64_t> commands_processed_{0};
  std::atomic<std::uint64_t> orders_processed_{0};
  std::atomic<std::uint64_t> market_events_processed_{0};
  std::atomic<std::uint64_t> fills_processed_{0};
  std::atomic<std::uint64_t> duplicate_commands_{0};
  std::atomic<std::uint64_t> failures_{0};
  std::atomic<std::int64_t> last_market_event_ms_{0};
  std::atomic<std::int64_t> last_successful_fill_ms_{0};
  mutable std::mutex latency_mutex_;
  std::vector<double> latency_samples_micros_;
};

}  // namespace simtrade::order
