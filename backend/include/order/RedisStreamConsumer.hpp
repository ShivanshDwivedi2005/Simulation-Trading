#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct redisContext;
struct redisReply;

namespace simtrade::order {

struct StreamEvent {
  std::string streamId;
  std::map<std::string, std::string> fields;
};

class RedisStreamConsumer {
 public:
  RedisStreamConsumer(std::string redis_url, std::string group, std::string consumer);
  ~RedisStreamConsumer();
  RedisStreamConsumer(const RedisStreamConsumer&) = delete;
  RedisStreamConsumer& operator=(const RedisStreamConsumer&) = delete;

  void ensure_group();
  [[nodiscard]] std::vector<StreamEvent> read(std::size_t count, std::chrono::milliseconds block);
  [[nodiscard]] std::vector<StreamEvent> recover(std::size_t count, std::chrono::milliseconds minimum_idle);
  void acknowledge(const std::vector<std::string>& stream_ids);
  [[nodiscard]] std::size_t pending_count();
  [[nodiscard]] bool delivered(const std::string& event_id);
  [[nodiscard]] bool mark_delivered(const std::string& event_id);
  [[nodiscard]] bool healthy();
  void reset_connection();

 private:
  struct Reply;

  [[nodiscard]] std::unique_ptr<Reply> command(const std::vector<std::string>& arguments);
  [[nodiscard]] std::vector<StreamEvent> parse_read_reply(const redisReply* reply) const;
  [[nodiscard]] bool connect();
  void disconnect();

  std::string redis_url_;
  std::string host_;
  std::string username_;
  std::string password_;
  int port_{6379};
  int database_{0};
  std::string group_;
  std::string consumer_;
  redisContext* context_{nullptr};
  std::recursive_mutex mutex_;
};

}  // namespace simtrade::order
