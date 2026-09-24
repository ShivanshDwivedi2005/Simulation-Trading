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

bool RedisClient::set(const std::string& key, const std::string& value) const {
  std::scoped_lock lock(mutex_);
  if (context_ == nullptr) return false;
  auto* reply = static_cast<redisReply*>(redisCommand(context_, "SET %b %b",
                                                       key.data(), key.size(),
                                                       value.data(), value.size()));
  const bool success = reply != nullptr && reply->type != REDIS_REPLY_ERROR;
  if (reply != nullptr) freeReplyObject(reply);
  return success;
}

std::optional<std::string> RedisClient::get(const std::string& key) const {
  std::scoped_lock lock(mutex_);
  if (context_ == nullptr) return std::nullopt;
  auto* reply = static_cast<redisReply*>(redisCommand(context_, "GET %b", key.data(), key.size()));
  if (reply == nullptr) return std::nullopt;
  std::optional<std::string> value;
  if (reply->type == REDIS_REPLY_STRING && reply->str != nullptr) {
    value = std::string(reply->str, static_cast<std::size_t>(reply->len));
  }
  freeReplyObject(reply);
  return value;
}

}  // namespace simtrade::cache
