#pragma once

#include <mutex>
#include <optional>
#include <string>

struct redisContext;

namespace simtrade::cache {

class RedisClient {
 public:
  explicit RedisClient(const std::string& redis_url);
  ~RedisClient();
  RedisClient(const RedisClient&) = delete;
  RedisClient& operator=(const RedisClient&) = delete;

  [[nodiscard]] bool healthy() const noexcept;
  [[nodiscard]] std::string status() const;
  [[nodiscard]] bool set(const std::string& key, const std::string& value) const;
  [[nodiscard]] std::optional<std::string> get(const std::string& key) const;

 private:
  redisContext* context_{nullptr};
  std::string error_;
  mutable std::mutex mutex_;
};

}  // namespace simtrade::cache
