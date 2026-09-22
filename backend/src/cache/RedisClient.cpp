#include "cache/RedisClient.hpp"

#include <hiredis/hiredis.h>

#include <chrono>
#include <regex>

namespace simtrade::cache {

RedisClient::RedisClient(const std::string& redis_url) {
  const std::regex pattern(R"(^redis://(?:[^@/]+@)?([^:/]+)(?::([0-9]+))?(?:/([0-9]+))?$)");
  std::smatch match;
  if (!std::regex_match(redis_url, match, pattern)) {
    error_ = "invalid REDIS_URL";
    return;
  }
  const auto host = match[1].str();
  const int port = match[2].matched ? std::stoi(match[2].str()) : 6379;
  const timeval timeout{1, 500000};
  context_ = redisConnectWithTimeout(host.c_str(), port, timeout);
  if (context_ == nullptr || context_->err != 0) {
    error_ = context_ == nullptr ? "connection allocation failed" : context_->errstr;
    if (context_ != nullptr) redisFree(context_);
    context_ = nullptr;
    return;
  }
  auto* reply = static_cast<redisReply*>(redisCommand(context_, "PING"));
  if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
    error_ = reply != nullptr && reply->str != nullptr ? reply->str : "PING failed";
    if (reply != nullptr) freeReplyObject(reply);
    redisFree(context_);
    context_ = nullptr;
    return;
  }
  freeReplyObject(reply);
}

RedisClient::~RedisClient() {
  if (context_ != nullptr) redisFree(context_);
}

bool RedisClient::healthy() const noexcept { return context_ != nullptr; }
std::string RedisClient::status() const { return healthy() ? "ok" : (error_.empty() ? "unavailable" : error_); }

}  // namespace simtrade::cache
