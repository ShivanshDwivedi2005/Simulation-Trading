#include "order/RedisStreamConsumer.hpp"

#include <hiredis/hiredis.h>

#include <cstdlib>
#include <stdexcept>

namespace simtrade::order {

struct RedisStreamConsumer::Reply {
  explicit Reply(redisReply* value) : value(value) {}
  ~Reply() { if (value != nullptr) freeReplyObject(value); }
  redisReply* value;
};

RedisStreamConsumer::RedisStreamConsumer(std::string redis_url, std::string group, std::string consumer)
    : redis_url_(std::move(redis_url)), group_(std::move(group)), consumer_(std::move(consumer)) {
  auto value = redis_url_;
  if (!value.starts_with("redis://")) throw std::invalid_argument("Redis URL must use redis://");
  value.erase(0, 8);
  const auto slash = value.find('/');
  auto authority = value.substr(0, slash);
  if (slash != std::string::npos && slash + 1 < value.size()) database_ = std::stoi(value.substr(slash + 1));
  const auto at = authority.rfind('@');
  if (at != std::string::npos) {
    const auto credentials = authority.substr(0, at);
    authority.erase(0, at + 1);
    const auto colon = credentials.find(':');
    if (colon == std::string::npos) password_ = credentials;
    else {
      username_ = credentials.substr(0, colon);
      password_ = credentials.substr(colon + 1);
    }
  }
  const auto colon = authority.rfind(':');
  host_ = colon == std::string::npos ? authority : authority.substr(0, colon);
  if (colon != std::string::npos) port_ = std::stoi(authority.substr(colon + 1));
  if (host_.empty() || database_ < 0) throw std::invalid_argument("Redis URL is invalid");
}

RedisStreamConsumer::~RedisStreamConsumer() { disconnect(); }

bool RedisStreamConsumer::connect() {
  disconnect();
  const timeval timeout{2, 0};
  context_ = redisConnectWithTimeout(host_.c_str(), port_, timeout);
  if (context_ == nullptr || context_->err != 0) {
    disconnect();
    return false;
  }
  if (!password_.empty()) {
    const auto auth = username_.empty() ? command({"AUTH", password_}) : command({"AUTH", username_, password_});
    if (auth->value->type == REDIS_REPLY_ERROR) {
      disconnect();
      return false;
    }
  }
  if (database_ != 0) {
    const auto select = command({"SELECT", std::to_string(database_)});
    if (select->value->type == REDIS_REPLY_ERROR) {
      disconnect();
      return false;
    }
  }
  return true;
}

void RedisStreamConsumer::disconnect() {
  if (context_ != nullptr) redisFree(context_);
  context_ = nullptr;
}

std::unique_ptr<RedisStreamConsumer::Reply> RedisStreamConsumer::command(
    const std::vector<std::string>& arguments) {
  std::scoped_lock lock(mutex_);
  if (context_ == nullptr && !connect()) throw std::runtime_error("Redis stream connection failed");
  std::vector<const char*> values;
  std::vector<std::size_t> lengths;
  values.reserve(arguments.size());
  lengths.reserve(arguments.size());
  for (const auto& argument : arguments) {
    values.push_back(argument.data());
    lengths.push_back(argument.size());
  }
  auto* raw = static_cast<redisReply*>(redisCommandArgv(context_, static_cast<int>(values.size()),
                                                       values.data(), lengths.data()));
  if (raw == nullptr) {
    disconnect();
    if (!connect()) throw std::runtime_error("Redis stream reconnect failed");
    raw = static_cast<redisReply*>(redisCommandArgv(context_, static_cast<int>(values.size()),
                                                   values.data(), lengths.data()));
  }
  if (raw == nullptr) throw std::runtime_error("Redis stream command failed");
  auto reply = std::make_unique<Reply>(raw);
  if (raw->type == REDIS_REPLY_ERROR) {
    const std::string error(raw->str == nullptr ? "Redis stream error" : raw->str,
                            raw->str == nullptr ? 18 : static_cast<std::size_t>(raw->len));
    throw std::runtime_error(error);
  }
  return reply;
}

void RedisStreamConsumer::ensure_group() {
  try {
    static_cast<void>(command({"XGROUP", "CREATE", "stream:order_events", group_, "0", "MKSTREAM"}));
  } catch (const std::exception& exception) {
    if (std::string(exception.what()).find("BUSYGROUP") == std::string::npos) throw;
  }
}

std::vector<StreamEvent> RedisStreamConsumer::parse_read_reply(const redisReply* reply) const {
  std::vector<StreamEvent> events;
  if (reply == nullptr || reply->type == REDIS_REPLY_NIL) return events;
  if (reply->type != REDIS_REPLY_ARRAY) throw std::runtime_error("invalid Redis stream read response");
  const redisReply* entries = reply;
  if (reply->elements == 1 && reply->element[0]->type == REDIS_REPLY_ARRAY &&
      reply->element[0]->elements == 2) {
    entries = reply->element[0]->element[1];
  } else if (reply->elements >= 2 && reply->element[1]->type == REDIS_REPLY_ARRAY) {
    entries = reply->element[1];
  }
  if (entries->type != REDIS_REPLY_ARRAY) return events;
  for (std::size_t index = 0; index < entries->elements; ++index) {
    const auto* entry = entries->element[index];
    if (entry == nullptr || entry->type != REDIS_REPLY_ARRAY || entry->elements != 2) continue;
    const auto* id = entry->element[0];
    const auto* fields = entry->element[1];
    if (id == nullptr || id->str == nullptr || fields == nullptr || fields->type != REDIS_REPLY_ARRAY) continue;
    StreamEvent event;
    event.streamId.assign(id->str, static_cast<std::size_t>(id->len));
    for (std::size_t field = 0; field + 1 < fields->elements; field += 2) {
      const auto* key = fields->element[field];
      const auto* value = fields->element[field + 1];
      if (key != nullptr && value != nullptr && key->str != nullptr && value->str != nullptr) {
        event.fields.emplace(std::string(key->str, static_cast<std::size_t>(key->len)),
                             std::string(value->str, static_cast<std::size_t>(value->len)));
      }
    }
    events.push_back(std::move(event));
  }
  return events;
}

std::vector<StreamEvent> RedisStreamConsumer::read(std::size_t count, std::chrono::milliseconds block) {
  const auto reply = command({"XREADGROUP", "GROUP", group_, consumer_, "COUNT", std::to_string(count),
                              "BLOCK", std::to_string(block.count()), "STREAMS", "stream:order_events", ">"});
  return parse_read_reply(reply->value);
}

std::vector<StreamEvent> RedisStreamConsumer::recover(std::size_t count,
                                                      std::chrono::milliseconds minimum_idle) {
  const auto reply = command({"XAUTOCLAIM", "stream:order_events", group_, consumer_,
                              std::to_string(minimum_idle.count()), "0-0", "COUNT", std::to_string(count)});
  return parse_read_reply(reply->value);
}

void RedisStreamConsumer::acknowledge(const std::vector<std::string>& stream_ids) {
  if (stream_ids.empty()) return;
  std::vector<std::string> arguments{"XACK", "stream:order_events", group_};
  arguments.insert(arguments.end(), stream_ids.begin(), stream_ids.end());
  const auto reply = command(arguments);
  if (reply->value->type != REDIS_REPLY_INTEGER) throw std::runtime_error("invalid Redis stream acknowledgement");
}

std::size_t RedisStreamConsumer::pending_count() {
  const auto reply = command({"XPENDING", "stream:order_events", group_});
  if (reply->value->type != REDIS_REPLY_ARRAY || reply->value->elements == 0 ||
      reply->value->element[0]->type != REDIS_REPLY_INTEGER) {
    throw std::runtime_error("invalid Redis pending response");
  }
  return static_cast<std::size_t>(reply->value->element[0]->integer);
}

bool RedisStreamConsumer::mark_delivered(const std::string& event_id) {
  const auto reply = command({"SET", "notification:" + event_id, "1", "NX", "EX", "604800"});
  return reply->value->type == REDIS_REPLY_STATUS;
}

bool RedisStreamConsumer::delivered(const std::string& event_id) {
  const auto reply = command({"EXISTS", "notification:" + event_id});
  return reply->value->type == REDIS_REPLY_INTEGER && reply->value->integer == 1;
}

bool RedisStreamConsumer::healthy() {
  try {
    const auto reply = command({"PING"});
    return reply->value->type == REDIS_REPLY_STATUS;
  } catch (const std::exception&) {
    return false;
  }
}

void RedisStreamConsumer::reset_connection() {
  std::scoped_lock lock(mutex_);
  disconnect();
}

}  // namespace simtrade::order
