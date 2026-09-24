#pragma once

#include "market/MarketDataCore.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace simtrade::market {

enum class WatchState {
    live,
    connecting,
    queued,
    invalid_symbol,
};

struct WatchResult {
    WatchState state = WatchState::invalid_symbol;
    std::optional<std::size_t> queue_position;
};

class ViewerSubscriptionSink {
public:
    virtual ~ViewerSubscriptionSink() = default;

    virtual WatchResult watch(const std::string& symbol) = 0;
    virtual WatchResult refresh(const std::string& symbol) = 0;
    virtual void unwatch(const std::string& symbol) = 0;
};

struct SymbolSubscription {
    std::string symbol;
    bool pinned = false;
    bool subscribed = false;
    bool subscribe_pending = false;
    bool unsubscribe_pending = false;

    std::size_t viewer_count = 0;
    std::size_t pending_order_count = 0;

    std::chrono::steady_clock::time_point subscribed_at{};
    std::chrono::steady_clock::time_point last_requested_at{};
    std::chrono::steady_clock::time_point last_viewer_left_at{};
};

struct QueuedSymbolSnapshot {
    std::string symbol;
    std::size_t position = 0;
    std::size_t viewer_demand = 0;
    std::size_t pending_order_demand = 0;
    std::chrono::milliseconds waiting_for{0};
    bool connecting = false;
};

class SubscriptionManager final : public ViewerSubscriptionSink {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using SymbolValidator = std::function<bool(const std::string&)>;
    using StatusHandler = std::function<void(
        const std::string& symbol,
        const std::string& status,
        bool live,
        std::optional<std::size_t> queue_position,
        const std::string& message)>;

    SubscriptionManager(
        SymbolSubscriptionSink& upstream,
        SymbolValidator validator,
        std::vector<std::string> pinned_symbols,
        std::size_t capacity,
        std::chrono::seconds eviction_grace,
        std::chrono::seconds minimum_residency,
        Clock clock = std::chrono::steady_clock::now);
    ~SubscriptionManager();

    SubscriptionManager(const SubscriptionManager&) = delete;
    SubscriptionManager& operator=(const SubscriptionManager&) = delete;

    WatchResult watch(const std::string& symbol) override;
    WatchResult refresh(const std::string& symbol) override;
    void unwatch(const std::string& symbol) override;

    WatchResult protect_order(const std::string& symbol);
    void release_order(const std::string& symbol);
    void restore_pending_orders(const std::map<std::string, std::size_t>& counts);

    void on_confirmation(std::set<std::string> confirmed_symbols);
    void on_connection_changed(bool connected);
    void set_status_handler(StatusHandler handler);

    std::vector<SymbolSubscription> snapshot() const;
    std::vector<QueuedSymbolSnapshot> queue_snapshot() const;
    std::optional<std::string> eviction_candidate() const;
    std::size_t active_symbol_count() const;
    std::size_t queued_symbol_count() const;
    std::size_t active_viewer_count() const;
    std::size_t eviction_count() const;
    std::size_t transition_count() const;

    // Useful for deterministic tests and diagnostics. Normal operation also
    // reevaluates the queue periodically on the serialized worker.
    void process_queue();

private:
    struct QueuedSymbol {
        std::string symbol;
        std::chrono::steady_clock::time_point queued_at{};
        std::size_t viewer_demand = 0;
        std::size_t pending_order_demand = 0;
    };

    using Task = std::function<void()>;

    template <typename Result, typename Fn>
    Result invoke(Fn&& fn) const {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        enqueue([promise, operation = std::forward<Fn>(fn)]() mutable {
            try {
                promise->set_value(operation());
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
        return future.get();
    }

    template <typename Fn>
    void invoke_void(Fn&& fn) const {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        enqueue([promise, operation = std::forward<Fn>(fn)]() mutable {
            try {
                operation();
                promise->set_value();
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
        future.get();
    }

    void enqueue(Task task) const;
    void run();
    void stop();

    WatchResult acquire_impl(const std::string& symbol, bool for_order);
    WatchResult refresh_impl(const std::string& symbol);
    void release_impl(const std::string& symbol, bool for_order);
    void handle_subscription_confirmation_impl(const std::set<std::string>& confirmed_symbols);
    void handle_connection_changed_impl(bool connected);

    bool begin_subscribe(SymbolSubscription& state);
    bool queue_needed(const SymbolSubscription& state) const;
    void update_waiting_entry(SymbolSubscription& state);
    void remove_waiting_entry_if_unused(const std::string& symbol);
    void process_queue_impl();
    std::vector<std::string> ordered_queue_symbols_impl() const;
    std::optional<std::size_t> queue_position_impl(const std::string& symbol) const;
    void notify_queue_positions();
    void notify_status(
        const std::string& symbol,
        const std::string& status,
        bool live,
        std::optional<std::size_t> queue_position,
        const std::string& message) const;

    std::size_t active_subscription_count_impl() const;
    std::optional<std::string> eviction_candidate_impl() const;
    bool evictable(const SymbolSubscription& state, std::chrono::steady_clock::time_point now) const;

    SymbolSubscriptionSink& upstream_;
    SymbolValidator validator_;
    const std::size_t capacity_;
    const std::chrono::seconds eviction_grace_;
    const std::chrono::seconds minimum_residency_;
    const Clock clock_;

    mutable std::mutex queue_mutex_;
    mutable std::condition_variable queue_condition_;
    mutable std::deque<Task> tasks_;
    bool stopping_ = false;
    std::thread worker_;

    std::map<std::string, SymbolSubscription> states_;
    std::map<std::string, QueuedSymbol> waiting_;
    std::optional<std::string> transition_target_;
    std::optional<std::string> eviction_victim_;
    bool connected_ = false;
    StatusHandler status_handler_;
    std::size_t eviction_count_ = 0;
    std::size_t transition_count_ = 0;
};

} // namespace simtrade::market
