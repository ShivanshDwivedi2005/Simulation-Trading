#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace simtrade::concurrency {

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) throw std::invalid_argument("queue capacity must be greater than zero");
  }

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  template <typename Rep, typename Period>
  bool push(T value, const std::chrono::duration<Rep, Period>& timeout) {
    std::unique_lock lock(mutex_);
    if (!not_full_.wait_for(lock, timeout, [this] { return closed_ || values_.size() < capacity_; })) return false;
    if (closed_) return false;
    values_.push_back(std::move(value));
    if (values_.size() > maximum_depth_) maximum_depth_ = values_.size();
    not_empty_.notify_one();
    return true;
  }

  bool pop(T& value) {
    std::unique_lock lock(mutex_);
    not_empty_.wait(lock, [this] { return closed_ || !values_.empty(); });
    if (values_.empty()) return false;
    value = std::move(values_.front());
    values_.pop_front();
    not_full_.notify_one();
    return true;
  }

  void close() {
    std::scoped_lock lock(mutex_);
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  [[nodiscard]] std::size_t size() const {
    std::scoped_lock lock(mutex_);
    return values_.size();
  }

  [[nodiscard]] std::size_t maximum_depth() const {
    std::scoped_lock lock(mutex_);
    return maximum_depth_;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  [[nodiscard]] bool closed() const {
    std::scoped_lock lock(mutex_);
    return closed_;
  }

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> values_;
  std::size_t maximum_depth_{};
  bool closed_{};
};

}  // namespace simtrade::concurrency
