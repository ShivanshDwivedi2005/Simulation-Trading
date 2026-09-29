#include "order/OrderEventWorkers.hpp"

#include "order/OrderRepository.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {

std::string field(const simtrade::order::StreamEvent& event, const std::string& name) {
  const auto found = event.fields.find(name);
  return found == event.fields.end() ? std::string{} : found->second;
}

std::vector<std::string> stream_ids(const std::vector<simtrade::order::StreamEvent>& events) {
  std::vector<std::string> ids;
  ids.reserve(events.size());
  for (const auto& event : events) ids.push_back(event.streamId);
  return ids;
}

}  // namespace

namespace simtrade::order {

NotificationWorker::NotificationWorker(std::string redis_url, DeliveryHandler delivery_handler,
                                       std::size_t batch_size)
    : consumer_(std::move(redis_url), "frontend_notifications", "frontend-1"),
      delivery_handler_(std::move(delivery_handler)), batch_size_(batch_size) {
  if (batch_size_ == 0 || batch_size_ > 1000) throw std::invalid_argument("notification batch size is invalid");
}

NotificationWorker::~NotificationWorker() { stop(); }

void NotificationWorker::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void NotificationWorker::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

bool NotificationWorker::deliver(const StreamEvent& event) {
  const auto event_type = field(event, "event_type");
  const bool notification = event_type == "ORDER_ADDED" || event_type == "PARTIALLY_FILLED" ||
                            event_type == "FILLED" || event_type == "ORDER_CANCELLED" ||
                            event_type == "ORDER_REJECTED" || event_type == "STOP_ACTIVATED";
  if (!notification) return true;
  const auto trader = field(event, "trader_id");
  const auto event_id = field(event, "event_id");
  if (trader.empty() || event_id.empty()) return true;
  if (consumer_.delivered(event_id)) return true;
  auto status = event_type;
  if (status == "ORDER_ADDED") status = "ACCEPTED";
  if (status == "ORDER_CANCELLED") status = "CANCELLED";
  if (status == "ORDER_REJECTED") status = "REJECTED";
  nlohmann::json payload{{"type", "order_update"}, {"event_id", event_id}, {"status", status}};
  for (const auto& [name, value] : event.fields) payload[name] = value;
  if (!delivery_handler_ || !delivery_handler_(std::stoull(trader), payload)) return false;
  static_cast<void>(consumer_.mark_delivered(event_id));
  return true;
}

void NotificationWorker::run() {
  try {
    consumer_.ensure_group();
    healthy_ = true;
    while (running_) {
      auto events = consumer_.recover(batch_size_, std::chrono::milliseconds(1000));
      if (events.empty()) events = consumer_.read(batch_size_, std::chrono::milliseconds(100));
      last_batch_size_ = events.size();
      std::vector<std::string> acknowledged;
      for (const auto& event : events) {
        try {
          if (deliver(event)) acknowledged.push_back(event.streamId);
        } catch (const std::exception&) {
          retry_count_.fetch_add(1);
        }
      }
      consumer_.acknowledge(acknowledged);
      processed_events_.fetch_add(acknowledged.size());
    }
  } catch (const std::exception& exception) {
    healthy_ = false;
    std::cerr << nlohmann::json({{"level", "error"}, {"event", "notification_worker_failed"},
                                 {"message", exception.what()}}).dump() << std::endl;
  }
  healthy_ = false;
}

EventWorkerHealth NotificationWorker::health() {
  std::size_t pending = 0;
  try { pending = consumer_.pending_count(); } catch (const std::exception&) { healthy_ = false; }
  return {running_, healthy_, pending, last_batch_size_, processed_events_, retry_count_};
}

PersistenceWorker::PersistenceWorker(std::string redis_url, std::string database_url,
                                     std::size_t batch_size)
    : consumer_(std::move(redis_url), "postgres_persistence", "persistence-1"),
      database_url_(std::move(database_url)), batch_size_(batch_size) {
  if (batch_size_ == 0 || batch_size_ > 1000) throw std::invalid_argument("persistence batch size is invalid");
}

PersistenceWorker::~PersistenceWorker() { stop(false); }

void PersistenceWorker::start() {
  if (running_.exchange(true)) return;
  drain_ = true;
  thread_ = std::thread([this] { run(); });
}

void PersistenceWorker::stop(bool drain) {
  drain_ = drain;
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

bool PersistenceWorker::persist(const std::vector<StreamEvent>& events) {
  std::uint32_t delay_ms = 10;
  while (true) {
    try {
      if (!repository_) repository_ = std::make_unique<OrderRepository>(database_url_);
      if (!repository_->persist_stream_events(events)) throw std::runtime_error("event batch was not committed");
      healthy_ = true;
      return true;
    } catch (const std::exception& exception) {
      repository_.reset();
      healthy_ = false;
      retry_count_.fetch_add(1);
      std::cerr << nlohmann::json({{"level", "warning"}, {"event", "persistence_batch_retry"},
                                   {"batch_size", events.size()}, {"message", exception.what()}}).dump() << std::endl;
      if (!running_) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      delay_ms = std::min<std::uint32_t>(delay_ms * 2, 1000);
    }
  }
}

void PersistenceWorker::run() {
  try {
    consumer_.ensure_group();
    healthy_ = true;
    bool empty_after_stop = false;
    while (running_ || (drain_ && !empty_after_stop)) {
      auto events = consumer_.recover(batch_size_, std::chrono::milliseconds(1000));
      if (events.empty()) events = consumer_.read(batch_size_, std::chrono::milliseconds(running_ ? 100 : 1));
      last_batch_size_ = events.size();
      if (events.empty()) {
        empty_after_stop = !running_;
        continue;
      }
      empty_after_stop = false;
      if (!persist(events)) break;
      consumer_.acknowledge(stream_ids(events));
      processed_events_.fetch_add(events.size());
    }
  } catch (const std::exception& exception) {
    healthy_ = false;
    std::cerr << nlohmann::json({{"level", "error"}, {"event", "persistence_worker_failed"},
                                 {"message", exception.what()}}).dump() << std::endl;
  }
  healthy_ = false;
}

EventWorkerHealth PersistenceWorker::health() {
  std::size_t pending = 0;
  try { pending = consumer_.pending_count(); } catch (const std::exception&) { healthy_ = false; }
  return {running_, healthy_, pending, last_batch_size_, processed_events_, retry_count_};
}

}  // namespace simtrade::order
