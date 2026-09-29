#pragma once

#include "order/RedisStreamConsumer.hpp"
#include "order/Order.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace simtrade::order {

class OrderRepository;

struct EventWorkerHealth {
  bool running{};
  bool healthy{};
  std::size_t pendingEvents{};
  std::size_t lastBatchSize{};
  std::uint64_t processedEvents{};
  std::uint64_t retryCount{};
};

class NotificationWorker {
 public:
  using DeliveryHandler = std::function<bool(TraderId, const nlohmann::json&)>;

  NotificationWorker(std::string redis_url, DeliveryHandler delivery_handler,
                     std::size_t batch_size = 100);
  ~NotificationWorker();
  NotificationWorker(const NotificationWorker&) = delete;
  NotificationWorker& operator=(const NotificationWorker&) = delete;

  void start();
  void stop();
  [[nodiscard]] EventWorkerHealth health();

 private:
  void run();
  [[nodiscard]] bool deliver(const StreamEvent& event);

  RedisStreamConsumer consumer_;
  DeliveryHandler delivery_handler_;
  std::size_t batch_size_;
  std::atomic<bool> running_{false};
  std::atomic<bool> healthy_{false};
  std::atomic<std::size_t> last_batch_size_{0};
  std::atomic<std::uint64_t> processed_events_{0};
  std::atomic<std::uint64_t> retry_count_{0};
  std::thread thread_;
};

class PersistenceWorker {
 public:
  PersistenceWorker(std::string redis_url, std::string database_url,
                    std::size_t batch_size = 100);
  ~PersistenceWorker();
  PersistenceWorker(const PersistenceWorker&) = delete;
  PersistenceWorker& operator=(const PersistenceWorker&) = delete;

  void start();
  void stop(bool drain = true);
  [[nodiscard]] EventWorkerHealth health();

 private:
  void run();
  [[nodiscard]] bool persist(const std::vector<StreamEvent>& events);

  RedisStreamConsumer consumer_;
  std::string database_url_;
  std::unique_ptr<OrderRepository> repository_;
  std::size_t batch_size_;
  std::atomic<bool> running_{false};
  std::atomic<bool> drain_{true};
  std::atomic<bool> healthy_{false};
  std::atomic<std::size_t> last_batch_size_{0};
  std::atomic<std::uint64_t> processed_events_{0};
  std::atomic<std::uint64_t> retry_count_{0};
  std::thread thread_;
};

}  // namespace simtrade::order
