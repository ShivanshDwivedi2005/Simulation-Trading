#pragma once

#include "market/MarketDataCore.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace simtrade::market {

enum class WatchResult { live, pending, capacity_full, invalid_symbol };

class ViewerSubscriptionSink {
 public:
  virtual ~ViewerSubscriptionSink() = default;
  virtual WatchResult watch(const std::string& symbol) = 0;
  virtual void unwatch(const std::string& symbol) = 0;
  virtual void touch(const std::string& symbol) = 0;
};

struct SymbolSubscription {
  std::string symbol;
  bool pinned{false};
  bool subscribed{false};
  bool subscribe_pending{false};
  bool unsubscribe_pending{false};
  std::size_t viewer_count{0};
  std::size_t pending_order_count{0};
  std::chrono::steady_clock::time_point subscribed_at{};
  std::chrono::steady_clock::time_point last_requested_at{};
  std::chrono::steady_clock::time_point last_viewer_left_at{};
};

class SubscriptionManager final : public ViewerSubscriptionSink {
 public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  using SymbolValidator = std::function<bool(const std::string&)>;
  using StatusHandler = std::function<void(const std::string&, const std::string&, bool, const std::string&)>;

  SubscriptionManager(SymbolSubscriptionSink& upstream,
                      SymbolValidator validator,
                      std::vector<std::string> pinned_symbols,
                      std::size_t maximum_symbols,
                      std::chrono::seconds eviction_grace,
                      std::chrono::seconds minimum_residency,
                      Clock clock = [] { return std::chrono::steady_clock::now(); });
  ~SubscriptionManager() override;
  SubscriptionManager(const SubscriptionManager&) = delete;
  SubscriptionManager& operator=(const SubscriptionManager&) = delete;

  WatchResult watch(const std::string& symbol) override;
  void unwatch(const std::string& symbol) override;
  void touch(const std::string& symbol) override;
  WatchResult protect_order(const std::string& symbol);
  void release_order(const std::string& symbol);
  void restore_pending_orders(const std::map<std::string, std::size_t>& counts);
  void on_confirmation(std::set<std::string> confirmed_symbols);
  void on_connection_changed(bool connected);
  void set_status_handler(StatusHandler handler);

  [[nodiscard]] std::vector<SymbolSubscription> snapshot() const;
  [[nodiscard]] std::size_t active_symbol_count() const;
  [[nodiscard]] std::optional<std::string> eviction_candidate() const;

 private:
  using Task = std::function<void()>;

  void enqueue(Task task) const;
  void run();
  void stop();
  WatchResult acquire(const std::string& raw_symbol, bool viewer, std::size_t order_count);
  void release(const std::string& raw_symbol, bool viewer, std::size_t order_count);
  WatchResult ensure_subscribed(SymbolSubscription& state);
  bool begin_subscribe(SymbolSubscription& state);
  void handle_confirmation(const std::set<std::string>& confirmed_symbols);
  void handle_connection_changed(bool connected);
  [[nodiscard]] std::size_t active_symbol_count_impl() const;
  [[nodiscard]] std::optional<std::string> eviction_candidate_impl() const;
  [[nodiscard]] bool evictable(const SymbolSubscription& state) const;
  void notify(const std::string& symbol,
              const std::string& status,
              bool live,
              const std::string& message) const;

  SymbolSubscriptionSink& upstream_;
  SymbolValidator validator_;
  std::size_t maximum_symbols_;
  std::chrono::seconds eviction_grace_;
  std::chrono::seconds minimum_residency_;
  Clock clock_;
  std::map<std::string, SymbolSubscription> states_;
  std::optional<std::string> replacement_target_;
  std::optional<std::string> eviction_victim_;
  StatusHandler status_handler_;
  bool connected_{false};

  mutable std::mutex queue_mutex_;
  mutable std::condition_variable queue_condition_;
  mutable std::deque<Task> tasks_;
  bool stopping_{false};
  std::thread worker_;
};

}  // namespace simtrade::market
