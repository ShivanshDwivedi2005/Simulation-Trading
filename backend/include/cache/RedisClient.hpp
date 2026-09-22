#pragma once

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

 private:
  redisContext* context_{nullptr};
  std::string error_;
};

}  // namespace simtrade::cache
