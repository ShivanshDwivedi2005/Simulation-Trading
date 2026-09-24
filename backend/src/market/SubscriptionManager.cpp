#include "market/SubscriptionManager.hpp"

#include <algorithm>
#include <future>
#include <stdexcept>
#include <utility>

namespace simtrade::market {

SubscriptionManager::SubscriptionManager(SymbolSubscriptionSink& upstream,
                                         SymbolValidator validator,
                                         std::vector<std::string> pinned_symbols,
                                         std::size_t maximum_symbols,
                                         std::chrono::seconds eviction_grace,
                                         std::chrono::seconds minimum_residency,
                                         Clock clock)
    : upstream_(upstream),
      validator_(std::move(validator)),
      maximum_symbols_(maximum_symbols),
      eviction_grace_(eviction_grace),
      minimum_residency_(minimum_residency),
      clock_(std::move(clock)) {
  if (maximum_symbols_ != 30) throw std::runtime_error("market-data capacity must be exactly 30 symbols");
  if (pinned_symbols.size() != 5) throw std::runtime_error("exactly five pinned symbols are required");

  std::set<std::string> unique;
  for (auto& raw_symbol : pinned_symbols) {
    const auto symbol = normalize_symbol(raw_symbol);
    if (!valid_symbol(symbol)) throw std::runtime_error("invalid pinned symbol syntax: " + raw_symbol);
    if (!unique.insert(symbol).second) throw std::runtime_error("duplicate pinned symbol: " + symbol);
    if (!validator_ || !validator_(symbol)) {
      throw std::runtime_error("pinned symbol is not present in the Alpaca instrument catalogue: " + symbol);
    }
  }

  worker_ = std::thread([this] { run(); });
  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();
  enqueue([this, pinned_symbols = std::move(pinned_symbols), promise] {
    try {
      for (const auto& raw_symbol : pinned_symbols) {
        const auto symbol = normalize_symbol(raw_symbol);
        auto& state = states_[symbol];
        state.symbol = symbol;
        state.pinned = true;
        state.last_requested_at = clock_();
        if (!begin_subscribe(state)) throw std::runtime_error("failed to reserve pinned symbol: " + symbol);
      }
      promise->set_value();
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
  });
  try {
    future.get();
  } catch (...) {
    stop();
    throw;
  }
}

SubscriptionManager::~SubscriptionManager() { stop(); }

WatchResult SubscriptionManager::watch(const std::string& symbol) {
  auto promise = std::make_shared<std::promise<WatchResult>>();
  auto future = promise->get_future();
  enqueue([this, symbol, promise] {
    try {
      promise->set_value(acquire(symbol, true, 0));
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
  });
  return future.get();
}

void SubscriptionManager::unwatch(const std::string& symbol) {
  enqueue([this, symbol] { release(symbol, true, 0); });
}

void SubscriptionManager::touch(const std::string& raw_symbol) {
  enqueue([this, raw_symbol] {
    const auto symbol = normalize_symbol(raw_symbol);
    const auto state = states_.find(symbol);
    if (state != states_.end()) state->second.last_requested_at = clock_();
  });
}

WatchResult SubscriptionManager::protect_order(const std::string& symbol) {
  auto promise = std::make_shared<std::promise<WatchResult>>();
  auto future = promise->get_future();
  enqueue([this, symbol, promise] {
    try {
      promise->set_value(acquire(symbol, false, 1));
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
  });
  return future.get();
}

void SubscriptionManager::release_order(const std::string& symbol) {
  enqueue([this, symbol] { release(symbol, false, 1); });
}

void SubscriptionManager::restore_pending_orders(const std::map<std::string, std::size_t>& counts) {
  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();
  enqueue([this, counts, promise] {
    try {
      for (const auto& [symbol, count] : counts) {
        if (count == 0) continue;
        const auto result = acquire(symbol, false, count);
        if (result == WatchResult::capacity_full) {
          throw std::runtime_error("persisted pending orders exceed market-data capacity at symbol: " + symbol);
        }
        if (result == WatchResult::invalid_symbol) {
          throw std::runtime_error("persisted pending order references an invalid catalogue symbol: " + symbol);
        }
      }
      promise->set_value();
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
  });
  future.get();
}

void SubscriptionManager::on_confirmation(std::set<std::string> confirmed_symbols) {
  enqueue([this, confirmed_symbols = std::move(confirmed_symbols)] { handle_confirmation(confirmed_symbols); });
}

void SubscriptionManager::on_connection_changed(bool connected) {
  enqueue([this, connected] { handle_connection_changed(connected); });
}

void SubscriptionManager::set_status_handler(StatusHandler handler) {
  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();
  enqueue([this, handler = std::move(handler), promise]() mutable {
    status_handler_ = std::move(handler);
    promise->set_value();
  });
  future.get();
}

std::vector<SymbolSubscription> SubscriptionManager::snapshot() const {
  auto promise = std::make_shared<std::promise<std::vector<SymbolSubscription>>>();
  auto future = promise->get_future();
  enqueue([this, promise] {
    std::vector<SymbolSubscription> result;
    result.reserve(states_.size());
    for (const auto& [symbol, state] : states_) {
      static_cast<void>(symbol);
      result.push_back(state);
    }
    promise->set_value(std::move(result));
  });
  return future.get();
}

std::size_t SubscriptionManager::active_symbol_count() const {
  auto promise = std::make_shared<std::promise<std::size_t>>();
  auto future = promise->get_future();
  enqueue([this, promise] { promise->set_value(active_symbol_count_impl()); });
  return future.get();
}

std::optional<std::string> SubscriptionManager::eviction_candidate() const {
  auto promise = std::make_shared<std::promise<std::optional<std::string>>>();
  auto future = promise->get_future();
  enqueue([this, promise] { promise->set_value(eviction_candidate_impl()); });
  return future.get();
}

void SubscriptionManager::enqueue(Task task) const {
  {
    std::scoped_lock lock(queue_mutex_);
    if (stopping_) throw std::runtime_error("subscription manager is stopping");
    tasks_.push_back(std::move(task));
  }
  queue_condition_.notify_one();
}

void SubscriptionManager::run() {
  for (;;) {
    Task task;
    {
      std::unique_lock lock(queue_mutex_);
      queue_condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty()) break;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
}

void SubscriptionManager::stop() {
  {
    std::scoped_lock lock(queue_mutex_);
    if (stopping_) return;
    stopping_ = true;
  }
  queue_condition_.notify_all();
  if (worker_.joinable()) worker_.join();
}

WatchResult SubscriptionManager::acquire(const std::string& raw_symbol,
                                        bool viewer,
                                        std::size_t order_count) {
  const auto symbol = normalize_symbol(raw_symbol);
  if (!valid_symbol(symbol) || !validator_ || !validator_(symbol)) return WatchResult::invalid_symbol;
  auto& state = states_[symbol];
  state.symbol = symbol;
  state.last_requested_at = clock_();

  if (state.unsubscribe_pending) return WatchResult::capacity_full;
  if (replacement_target_ && *replacement_target_ != symbol &&
      !state.subscribed && !state.subscribe_pending) {
    return WatchResult::capacity_full;
  }

  if (viewer) ++state.viewer_count;
  state.pending_order_count += order_count;
  const auto result = ensure_subscribed(state);
  if (result == WatchResult::capacity_full || result == WatchResult::invalid_symbol) {
    if (viewer && state.viewer_count > 0) --state.viewer_count;
    state.pending_order_count -= std::min(state.pending_order_count, order_count);
  }
  return result;
}

void SubscriptionManager::release(const std::string& raw_symbol,
                                  bool viewer,
                                  std::size_t order_count) {
  const auto symbol = normalize_symbol(raw_symbol);
  const auto found = states_.find(symbol);
  if (found == states_.end()) return;
  auto& state = found->second;
  if (viewer && state.viewer_count > 0) {
    --state.viewer_count;
    if (state.viewer_count == 0) state.last_viewer_left_at = clock_();
  }
  if (order_count > 0) {
    state.pending_order_count -= std::min(state.pending_order_count, order_count);
    if (state.pending_order_count == 0 && state.viewer_count == 0) state.last_viewer_left_at = clock_();
  }
  if (replacement_target_ && *replacement_target_ == symbol &&
      state.viewer_count == 0 && state.pending_order_count == 0) {
    replacement_target_.reset();
  }
}

WatchResult SubscriptionManager::ensure_subscribed(SymbolSubscription& state) {
  if (state.subscribed) return WatchResult::live;
  if (state.subscribe_pending || (replacement_target_ && *replacement_target_ == state.symbol)) {
    return WatchResult::pending;
  }
  if (active_symbol_count_impl() < maximum_symbols_) {
    return begin_subscribe(state) ? WatchResult::pending : WatchResult::capacity_full;
  }
  if (replacement_target_) return WatchResult::capacity_full;

  const auto victim = eviction_candidate_impl();
  if (!victim) return WatchResult::capacity_full;
  auto& victim_state = states_.at(*victim);
  victim_state.unsubscribe_pending = true;
  replacement_target_ = state.symbol;
  eviction_victim_ = *victim;
  if (!upstream_.unsubscribe(*victim)) {
    victim_state.unsubscribe_pending = false;
    replacement_target_.reset();
    eviction_victim_.reset();
    return WatchResult::capacity_full;
  }
  return WatchResult::pending;
}

bool SubscriptionManager::begin_subscribe(SymbolSubscription& state) {
  if (state.subscribe_pending || state.subscribed || state.unsubscribe_pending) return true;
  state.subscribe_pending = true;
  const auto result = upstream_.subscribe(state.symbol);
  if (result == SubscriptionResult::added || result == SubscriptionResult::duplicate) return true;
  state.subscribe_pending = false;
  return false;
}

void SubscriptionManager::handle_confirmation(const std::set<std::string>& confirmed_symbols) {
  const auto now = clock_();
  for (auto& [symbol, state] : states_) {
    const bool confirmed = confirmed_symbols.contains(symbol);
    if (state.unsubscribe_pending && !confirmed) {
      state.unsubscribe_pending = false;
      state.subscribed = false;
    }
    if (confirmed) {
      if (!state.subscribed) state.subscribed_at = now;
      state.subscribed = true;
      if (state.subscribe_pending) {
        state.subscribe_pending = false;
        notify(symbol, "LIVE", true, "Live market data is available.");
      }
    } else if (!state.unsubscribe_pending) {
      state.subscribed = false;
    }
  }

  if (eviction_victim_) {
    const auto victim = states_.find(*eviction_victim_);
    if (victim == states_.end() || !victim->second.unsubscribe_pending) {
      eviction_victim_.reset();
      if (replacement_target_) {
        auto& replacement = states_.at(*replacement_target_);
        if (replacement.viewer_count > 0 || replacement.pending_order_count > 0 || replacement.pinned) {
          if (!begin_subscribe(replacement)) {
            notify(replacement.symbol,
                   "CAPACITY_FULL",
                   false,
                   "All live market-data slots are currently in use.");
          }
        }
        replacement_target_.reset();
      }
    }
  }
}

void SubscriptionManager::handle_connection_changed(bool connected) {
  connected_ = connected;
  if (connected) return;
  for (auto& [symbol, state] : states_) {
    static_cast<void>(symbol);
    state.subscribed = false;
    if (!state.unsubscribe_pending &&
        (state.pinned || state.viewer_count > 0 || state.pending_order_count > 0 || state.subscribe_pending)) {
      state.subscribe_pending = true;
    }
  }
}

std::size_t SubscriptionManager::active_symbol_count_impl() const {
  return static_cast<std::size_t>(std::count_if(states_.begin(), states_.end(), [](const auto& item) {
    const auto& state = item.second;
    return state.subscribed || state.subscribe_pending || state.unsubscribe_pending;
  }));
}

std::optional<std::string> SubscriptionManager::eviction_candidate_impl() const {
  const SymbolSubscription* oldest = nullptr;
  for (const auto& [symbol, state] : states_) {
    static_cast<void>(symbol);
    if (!evictable(state)) continue;
    if (oldest == nullptr || state.last_requested_at < oldest->last_requested_at) oldest = &state;
  }
  return oldest == nullptr ? std::nullopt : std::optional<std::string>(oldest->symbol);
}

bool SubscriptionManager::evictable(const SymbolSubscription& state) const {
  const auto now = clock_();
  return state.subscribed && !state.pinned && state.viewer_count == 0 && state.pending_order_count == 0 &&
         !state.subscribe_pending && !state.unsubscribe_pending &&
         state.last_viewer_left_at != std::chrono::steady_clock::time_point{} &&
         state.subscribed_at != std::chrono::steady_clock::time_point{} &&
         now - state.last_viewer_left_at >= eviction_grace_ &&
         now - state.subscribed_at >= minimum_residency_;
}

void SubscriptionManager::notify(const std::string& symbol,
                                 const std::string& status,
                                 bool live,
                                 const std::string& message) const {
  if (status_handler_) status_handler_(symbol, status, live, message);
}

}  // namespace simtrade::market
