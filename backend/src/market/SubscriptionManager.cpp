#include "market/SubscriptionManager.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace simtrade::market {

namespace {
constexpr auto kQueuePollInterval = std::chrono::milliseconds(250);
}  // namespace

SubscriptionManager::SubscriptionManager(SymbolSubscriptionSink& upstream,
                                         SymbolValidator validator,
                                         std::vector<std::string> pinned_symbols,
                                         std::size_t capacity,
                                         std::chrono::seconds eviction_grace,
                                         std::chrono::seconds minimum_residency,
                                         Clock clock)
    : upstream_(upstream),
      validator_(std::move(validator)),
      capacity_(capacity),
      eviction_grace_(eviction_grace),
      minimum_residency_(minimum_residency),
      clock_(std::move(clock)) {
  if (capacity_ != 30) throw std::runtime_error("market-data capacity must be exactly 30 symbols");
  if (pinned_symbols.size() != 5) throw std::runtime_error("exactly five pinned symbols are required");

  std::set<std::string> unique;
  for (const auto& raw_symbol : pinned_symbols) {
    const auto symbol = normalize_symbol(raw_symbol);
    if (!valid_symbol(symbol)) throw std::runtime_error("invalid pinned symbol syntax: " + raw_symbol);
    if (!unique.insert(symbol).second) throw std::runtime_error("duplicate pinned symbol: " + symbol);
    if (!validator_ || !validator_(symbol)) {
      throw std::runtime_error("pinned symbol is not present in the Alpaca instrument catalogue: " + symbol);
    }
  }

  worker_ = std::thread([this] { run(); });
  try {
    invoke_void([this, pinned_symbols = std::move(pinned_symbols)] {
      for (const auto& raw_symbol : pinned_symbols) {
        const auto symbol = normalize_symbol(raw_symbol);
        auto& state = states_[symbol];
        state.symbol = symbol;
        state.pinned = true;
        state.last_requested_at = clock_();
        if (!begin_subscribe(state)) throw std::runtime_error("failed to reserve pinned symbol: " + symbol);
      }
    });
  } catch (...) {
    stop();
    throw;
  }
}

SubscriptionManager::~SubscriptionManager() { stop(); }

WatchResult SubscriptionManager::watch(const std::string& symbol) {
  return invoke<WatchResult>([this, symbol] { return acquire_impl(symbol, false); });
}

WatchResult SubscriptionManager::refresh(const std::string& symbol) {
  return invoke<WatchResult>([this, symbol] { return refresh_impl(symbol); });
}

void SubscriptionManager::unwatch(const std::string& symbol) {
  enqueue([this, symbol] { release_impl(symbol, false); });
}

WatchResult SubscriptionManager::protect_order(const std::string& symbol) {
  return invoke<WatchResult>([this, symbol] { return acquire_impl(symbol, true); });
}

void SubscriptionManager::release_order(const std::string& symbol) {
  enqueue([this, symbol] { release_impl(symbol, true); });
}

void SubscriptionManager::restore_pending_orders(const std::map<std::string, std::size_t>& counts) {
  invoke_void([this, counts] {
    for (const auto& [raw_symbol, count] : counts) {
      const auto symbol = normalize_symbol(raw_symbol);
      if (count == 0) continue;
      if (!valid_symbol(symbol) || !validator_ || !validator_(symbol)) {
        throw std::runtime_error("persisted pending order references an invalid catalogue symbol: " + raw_symbol);
      }
      auto& state = states_[symbol];
      state.symbol = symbol;
      state.pending_order_count += count;
      state.last_requested_at = clock_();
      if (!state.subscribed && !state.subscribe_pending) update_waiting_entry(state);
    }
    process_queue_impl();
    notify_queue_positions();
  });
}

void SubscriptionManager::on_confirmation(std::set<std::string> confirmed_symbols) {
  enqueue([this, confirmed_symbols = std::move(confirmed_symbols)] {
    handle_subscription_confirmation_impl(confirmed_symbols);
  });
}

void SubscriptionManager::on_connection_changed(bool connected) {
  enqueue([this, connected] { handle_connection_changed_impl(connected); });
}

void SubscriptionManager::set_status_handler(StatusHandler handler) {
  invoke_void([this, handler = std::move(handler)]() mutable { status_handler_ = std::move(handler); });
}

std::vector<SymbolSubscription> SubscriptionManager::snapshot() const {
  return invoke<std::vector<SymbolSubscription>>([this] {
    std::vector<SymbolSubscription> result;
    result.reserve(states_.size());
    for (const auto& [symbol, state] : states_) {
      static_cast<void>(symbol);
      result.push_back(state);
    }
    return result;
  });
}

std::vector<QueuedSymbolSnapshot> SubscriptionManager::queue_snapshot() const {
  return invoke<std::vector<QueuedSymbolSnapshot>>([this] {
    const auto now = clock_();
    const auto ordered = ordered_queue_symbols_impl();
    std::vector<QueuedSymbolSnapshot> result;
    result.reserve(ordered.size());
    for (std::size_t index = 0; index < ordered.size(); ++index) {
      const auto& queued = waiting_.at(ordered[index]);
      result.push_back(QueuedSymbolSnapshot{queued.symbol,
                                            index + 1,
                                            queued.viewer_demand,
                                            queued.pending_order_demand,
                                            std::chrono::duration_cast<std::chrono::milliseconds>(now - queued.queued_at),
                                            transition_target_ && *transition_target_ == queued.symbol});
    }
    return result;
  });
}

std::optional<std::string> SubscriptionManager::eviction_candidate() const {
  return invoke<std::optional<std::string>>([this] { return eviction_candidate_impl(); });
}

std::size_t SubscriptionManager::active_symbol_count() const {
  return invoke<std::size_t>([this] { return active_subscription_count_impl(); });
}

std::size_t SubscriptionManager::queued_symbol_count() const {
  return invoke<std::size_t>([this] { return waiting_.size(); });
}

std::size_t SubscriptionManager::active_viewer_count() const {
  return invoke<std::size_t>([this] {
    std::size_t count = 0;
    for (const auto& [symbol, state] : states_) {
      static_cast<void>(symbol);
      count += state.viewer_count;
    }
    return count;
  });
}

std::size_t SubscriptionManager::eviction_count() const {
  return invoke<std::size_t>([this] { return eviction_count_; });
}

std::size_t SubscriptionManager::transition_count() const {
  return invoke<std::size_t>([this] { return transition_count_; });
}

void SubscriptionManager::process_queue() {
  invoke_void([this] {
    process_queue_impl();
    notify_queue_positions();
  });
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
      queue_condition_.wait_for(lock, kQueuePollInterval, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty()) break;
      if (!tasks_.empty()) {
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
    }
    if (task) {
      task();
    } else {
      process_queue_impl();
      notify_queue_positions();
    }
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

WatchResult SubscriptionManager::acquire_impl(const std::string& raw_symbol, bool for_order) {
  const auto symbol = normalize_symbol(raw_symbol);
  if (!valid_symbol(symbol) || !validator_ || !validator_(symbol)) {
    return {WatchState::invalid_symbol, std::nullopt};
  }

  auto& state = states_[symbol];
  state.symbol = symbol;
  state.last_requested_at = clock_();
  if (for_order) {
    ++state.pending_order_count;
  } else {
    ++state.viewer_count;
  }

  if (state.subscribed && !state.unsubscribe_pending) {
    waiting_.erase(symbol);
    return {WatchState::live, std::nullopt};
  }

  update_waiting_entry(state);
  process_queue_impl();
  notify_queue_positions();
  if (state.subscribe_pending || (transition_target_ && *transition_target_ == symbol)) {
    return {WatchState::connecting, std::nullopt};
  }
  return {WatchState::queued, queue_position_impl(symbol)};
}

WatchResult SubscriptionManager::refresh_impl(const std::string& raw_symbol) {
  const auto symbol = normalize_symbol(raw_symbol);
  const auto found = states_.find(symbol);
  if (found == states_.end()) return {WatchState::invalid_symbol, std::nullopt};
  auto& state = found->second;
  state.last_requested_at = clock_();
  if (state.subscribed && !state.unsubscribe_pending) return {WatchState::live, std::nullopt};
  update_waiting_entry(state);
  process_queue_impl();
  if (state.subscribe_pending || (transition_target_ && *transition_target_ == symbol)) {
    return {WatchState::connecting, std::nullopt};
  }
  return {WatchState::queued, queue_position_impl(symbol)};
}

void SubscriptionManager::release_impl(const std::string& raw_symbol, bool for_order) {
  const auto symbol = normalize_symbol(raw_symbol);
  const auto found = states_.find(symbol);
  if (found == states_.end()) return;
  auto& state = found->second;
  if (for_order) {
    if (state.pending_order_count > 0) --state.pending_order_count;
  } else if (state.viewer_count > 0) {
    --state.viewer_count;
  }
  if (state.viewer_count == 0 && state.pending_order_count == 0) state.last_viewer_left_at = clock_();

  if (queue_needed(state)) {
    update_waiting_entry(state);
  } else {
    waiting_.erase(symbol);
  }
  process_queue_impl();
  notify_queue_positions();
}

bool SubscriptionManager::begin_subscribe(SymbolSubscription& state) {
  if (state.subscribe_pending || state.subscribed || state.unsubscribe_pending) return true;
  state.subscribe_pending = true;
  const auto result = upstream_.subscribe(state.symbol);
  if (result == SubscriptionResult::added || result == SubscriptionResult::duplicate) {
    ++transition_count_;
    return true;
  }
  state.subscribe_pending = false;
  return false;
}

bool SubscriptionManager::queue_needed(const SymbolSubscription& state) const {
  return state.pinned || state.viewer_count > 0 || state.pending_order_count > 0;
}

void SubscriptionManager::update_waiting_entry(SymbolSubscription& state) {
  if (!queue_needed(state) || state.subscribed) {
    waiting_.erase(state.symbol);
    return;
  }
  const auto [iterator, inserted] = waiting_.try_emplace(
      state.symbol, QueuedSymbol{state.symbol, clock_(), state.viewer_count, state.pending_order_count});
  if (!inserted) {
    iterator->second.viewer_demand = state.viewer_count;
    iterator->second.pending_order_demand = state.pending_order_count;
  }
}

void SubscriptionManager::remove_waiting_entry_if_unused(const std::string& symbol) {
  const auto state = states_.find(symbol);
  if (state == states_.end() || !queue_needed(state->second) || state->second.subscribed) waiting_.erase(symbol);
}

void SubscriptionManager::process_queue_impl() {
  for (auto iterator = waiting_.begin(); iterator != waiting_.end();) {
    const auto state = states_.find(iterator->first);
    if (state == states_.end() || !queue_needed(state->second) || state->second.subscribed) {
      iterator = waiting_.erase(iterator);
    } else {
      iterator->second.viewer_demand = state->second.viewer_count;
      iterator->second.pending_order_demand = state->second.pending_order_count;
      ++iterator;
    }
  }

  if (transition_target_ || eviction_victim_ || waiting_.empty()) return;
  const auto ordered = ordered_queue_symbols_impl();
  if (ordered.empty()) return;

  const auto target_symbol = ordered.front();
  auto& target = states_.at(target_symbol);
  if (!queue_needed(target)) {
    waiting_.erase(target_symbol);
    return;
  }

  if (active_subscription_count_impl() < capacity_) {
    if (begin_subscribe(target)) {
      waiting_.erase(target_symbol);
      notify_status(target_symbol, "CONNECTING", false, std::nullopt, "Connecting to live market data.");
      process_queue_impl();
      return;
    }
    notify_status(target_symbol, "ERROR", false, queue_position_impl(target_symbol),
                  "Unable to request live market data.");
    return;
  }

  if (!connected_) {
    return;
  }
  const auto victim = eviction_candidate_impl();
  if (!victim) {
    return;
  }

  transition_target_ = target_symbol;
  auto& victim_state = states_.at(*victim);
  victim_state.unsubscribe_pending = true;
  eviction_victim_ = *victim;
  if (!upstream_.unsubscribe(*victim)) {
    victim_state.unsubscribe_pending = false;
    eviction_victim_.reset();
    transition_target_.reset();
    notify_status(target_symbol, "ERROR", false, queue_position_impl(target_symbol),
                  "Unable to release a live market-data slot.");
    return;
  }
  ++eviction_count_;
  ++transition_count_;
  notify_status(target_symbol, "CONNECTING", false, std::nullopt, "Preparing a live market-data slot.");
}

std::vector<std::string> SubscriptionManager::ordered_queue_symbols_impl() const {
  std::vector<const QueuedSymbol*> ordered;
  ordered.reserve(waiting_.size());
  for (const auto& [symbol, queued] : waiting_) {
    static_cast<void>(symbol);
    ordered.push_back(&queued);
  }
  std::sort(ordered.begin(), ordered.end(), [](const QueuedSymbol* left, const QueuedSymbol* right) {
    const bool left_order = left->pending_order_demand > 0;
    const bool right_order = right->pending_order_demand > 0;
    if (left_order != right_order) return left_order > right_order;
    if (left->viewer_demand != right->viewer_demand) return left->viewer_demand > right->viewer_demand;
    if (left->queued_at != right->queued_at) return left->queued_at < right->queued_at;
    return left->symbol < right->symbol;
  });

  std::vector<std::string> result;
  result.reserve(ordered.size());
  for (const auto* queued : ordered) result.push_back(queued->symbol);
  return result;
}

std::optional<std::size_t> SubscriptionManager::queue_position_impl(const std::string& symbol) const {
  const auto ordered = ordered_queue_symbols_impl();
  const auto found = std::find(ordered.begin(), ordered.end(), symbol);
  if (found == ordered.end()) return std::nullopt;
  return static_cast<std::size_t>(std::distance(ordered.begin(), found)) + 1;
}

void SubscriptionManager::notify_queue_positions() {
  const auto ordered = ordered_queue_symbols_impl();
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    const auto& symbol = ordered[index];
    if (transition_target_ && *transition_target_ == symbol) continue;
    notify_status(symbol,
                  "QUEUED",
                  false,
                  index + 1,
                  "Live data is currently at capacity. This stock is queued and the latest available snapshot is being shown.");
  }
}

void SubscriptionManager::handle_subscription_confirmation_impl(const std::set<std::string>& confirmed_symbols) {
  const auto now = clock_();
  std::set<std::string> newly_live;
  for (auto& [symbol, state] : states_) {
    const bool confirmed = confirmed_symbols.contains(symbol);
    const bool was_subscribe_pending = state.subscribe_pending;
    if (state.unsubscribe_pending && !confirmed) {
      state.unsubscribe_pending = false;
      state.subscribed = false;
    }
    if (confirmed) {
      if (!state.subscribed) state.subscribed_at = now;
      state.subscribed = true;
      if (state.subscribe_pending) state.subscribe_pending = false;
      if (was_subscribe_pending) newly_live.insert(symbol);
    } else if (!state.unsubscribe_pending) {
      state.subscribed = false;
    }
  }

  if (eviction_victim_) {
    const auto victim = states_.find(*eviction_victim_);
    if (victim == states_.end() || !victim->second.unsubscribe_pending) {
      eviction_victim_.reset();
      if (transition_target_) {
        auto& target = states_.at(*transition_target_);
        if (queue_needed(target)) {
          if (begin_subscribe(target)) {
            notify_status(target.symbol, "CONNECTING", false, std::nullopt, "Connecting to live market data.");
          } else {
            notify_status(target.symbol, "ERROR", false, queue_position_impl(target.symbol),
                          "Unable to request live market data.");
            transition_target_.reset();
          }
        } else {
          waiting_.erase(target.symbol);
          transition_target_.reset();
        }
      }
    }
  }

  if (transition_target_) {
    auto& target = states_.at(*transition_target_);
    if (target.subscribed && !target.subscribe_pending) {
      const auto target_symbol = target.symbol;
      if (queue_needed(target)) {
        waiting_.erase(target_symbol);
        notify_status(target_symbol, "LIVE", true, std::nullopt, "Live market data connected.");
        newly_live.erase(target_symbol);
        transition_target_.reset();
      } else {
        waiting_.erase(target_symbol);
        transition_target_.reset();
        target.unsubscribe_pending = true;
        eviction_victim_ = target_symbol;
        if (!upstream_.unsubscribe(target_symbol)) {
          target.unsubscribe_pending = false;
          eviction_victim_.reset();
        } else {
          ++transition_count_;
        }
      }
    }
  }

  for (const auto& symbol : newly_live) {
    waiting_.erase(symbol);
    const auto state = states_.find(symbol);
    if (state != states_.end() && queue_needed(state->second)) {
      notify_status(symbol, "LIVE", true, std::nullopt, "Live market data connected.");
    }
  }

  process_queue_impl();
  notify_queue_positions();
}

void SubscriptionManager::handle_connection_changed_impl(bool connected) {
  connected_ = connected;
  if (connected) {
    process_queue_impl();
    notify_queue_positions();
    return;
  }

  transition_target_.reset();
  eviction_victim_.reset();
  for (auto& [symbol, state] : states_) {
    const bool was_allocated = state.subscribed || state.subscribe_pending || state.unsubscribe_pending;
    state.subscribed = false;
    state.unsubscribe_pending = false;
    if (was_allocated && queue_needed(state)) {
      state.subscribe_pending = true;
    } else if (was_allocated) {
      state.subscribe_pending = false;
      upstream_.unsubscribe(symbol);
    }
    if (!state.subscribe_pending && queue_needed(state)) update_waiting_entry(state);
  }
  notify_queue_positions();
}

std::size_t SubscriptionManager::active_subscription_count_impl() const {
  return static_cast<std::size_t>(std::count_if(states_.begin(), states_.end(), [](const auto& item) {
    const auto& state = item.second;
    return state.subscribed || state.subscribe_pending || state.unsubscribe_pending;
  }));
}

std::optional<std::string> SubscriptionManager::eviction_candidate_impl() const {
  const auto now = clock_();
  const SymbolSubscription* oldest = nullptr;
  for (const auto& [symbol, state] : states_) {
    static_cast<void>(symbol);
    if (!evictable(state, now)) continue;
    if (oldest == nullptr || state.last_requested_at < oldest->last_requested_at) oldest = &state;
  }
  return oldest == nullptr ? std::nullopt : std::optional<std::string>(oldest->symbol);
}

bool SubscriptionManager::evictable(const SymbolSubscription& state,
                                    std::chrono::steady_clock::time_point now) const {
  return state.subscribed && !state.pinned && state.viewer_count == 0 && state.pending_order_count == 0 &&
         !state.subscribe_pending && !state.unsubscribe_pending &&
         state.last_viewer_left_at != std::chrono::steady_clock::time_point{} &&
         state.subscribed_at != std::chrono::steady_clock::time_point{} &&
         now - state.last_viewer_left_at >= eviction_grace_ &&
         now - state.subscribed_at >= minimum_residency_;
}

void SubscriptionManager::notify_status(const std::string& symbol,
                                        const std::string& status,
                                        bool live,
                                        std::optional<std::size_t> queue_position,
                                        const std::string& message) const {
  if (status_handler_) status_handler_(symbol, status, live, queue_position, message);
}

}  // namespace simtrade::market
