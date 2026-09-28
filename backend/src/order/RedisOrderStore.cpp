#include "order/RedisOrderStore.hpp"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

constexpr auto active_status = R"(
local status = redis.call('HGET', KEYS[1], 'status')
if status ~= 'PENDING' and status ~= 'OPEN' and status ~= 'PARTIALLY_FILLED' then return -2 end
)";

const std::string add_order_script = R"(
local existed = redis.call('EXISTS', KEYS[1])
if existed == 1 then
  local current = tonumber(redis.call('HGET', KEYS[1], 'version') or '-1')
  if current > tonumber(ARGV[13]) then return -2 end
  local oldKey = redis.call('HGET', KEYS[1], 'book_key') or ''
  local oldMember = redis.call('HGET', KEYS[1], 'book_member') or ''
  if oldKey ~= '' and (oldKey ~= ARGV[16] or oldMember ~= ARGV[12]) then redis.call('ZREM', oldKey, oldMember) end
end
redis.call('HSET', KEYS[1],
  'trader_id', ARGV[2], 'instrument_id', ARGV[3], 'side', ARGV[4], 'type', ARGV[5],
  'status', ARGV[6], 'limit_price_ticks', ARGV[7], 'stop_price_ticks', ARGV[8],
  'quantity', ARGV[9], 'remaining_quantity', ARGV[10], 'sequence_number', ARGV[11],
  'book_member', ARGV[12], 'version', ARGV[13], 'book_key', ARGV[16], 'stop_activated', ARGV[17])
redis.call('SADD', KEYS[2], ARGV[1])
if ARGV[16] ~= '' then redis.call('ZADD', KEYS[3], ARGV[14], ARGV[12]) end
if existed == 0 then
  redis.call('XADD', KEYS[4], '*', 'event_id', ARGV[15], 'event_type', 'ORDER_ADDED',
    'order_id', ARGV[1], 'trader_id', ARGV[2], 'instrument_id', ARGV[3], 'version', ARGV[13])
end
return existed == 0 and 1 or 0
)";

const std::string cancel_order_script = std::string(R"(
if redis.call('EXISTS', KEYS[1]) == 0 then return -1 end
)") + active_status + R"(
if redis.call('HGET', KEYS[1], 'trader_id') ~= ARGV[2] then return -3 end
if tonumber(redis.call('HGET', KEYS[1], 'version')) ~= tonumber(ARGV[3]) then return -4 end
local bookKey = redis.call('HGET', KEYS[1], 'book_key') or ''
local member = redis.call('HGET', KEYS[1], 'book_member') or ''
if bookKey ~= '' then redis.call('ZREM', bookKey, member) end
redis.call('SREM', 'trader:' .. ARGV[2] .. ':open_orders', ARGV[1])
redis.call('DEL', KEYS[1])
redis.call('XADD', KEYS[2], '*', 'event_id', ARGV[4], 'event_type', 'ORDER_CANCELLED',
  'order_id', ARGV[1], 'trader_id', ARGV[2], 'version', tostring(tonumber(ARGV[3]) + 1))
return 1
)";

const std::string modify_quantity_script = std::string(R"(
if redis.call('EXISTS', KEYS[1]) == 0 then return -1 end
)") + active_status + R"(
if redis.call('HGET', KEYS[1], 'trader_id') ~= ARGV[2] then return -3 end
if tonumber(redis.call('HGET', KEYS[1], 'version')) ~= tonumber(ARGV[5]) then return -4 end
local oldQuantity = tonumber(redis.call('HGET', KEYS[1], 'quantity'))
local oldRemaining = tonumber(redis.call('HGET', KEYS[1], 'remaining_quantity'))
local filled = oldQuantity - oldRemaining
local newQuantity = tonumber(ARGV[3])
if newQuantity <= 0 or newQuantity < filled then return -5 end
local newRemaining = newQuantity - filled
local sequence = redis.call('HGET', KEYS[1], 'sequence_number')
local member = redis.call('HGET', KEYS[1], 'book_member')
local bookKey = redis.call('HGET', KEYS[1], 'book_key') or ''
if newQuantity > oldQuantity then
  if tonumber(ARGV[4]) <= tonumber(sequence) then return -6 end
  if bookKey ~= '' then redis.call('ZREM', bookKey, member) end
  sequence = ARGV[4]
  member = ARGV[7]
  if bookKey ~= '' then redis.call('ZADD', bookKey, ARGV[8], member) end
end
local version = tonumber(ARGV[5]) + 1
redis.call('HSET', KEYS[1], 'quantity', ARGV[3], 'remaining_quantity', tostring(newRemaining),
  'sequence_number', sequence, 'book_member', member, 'version', tostring(version))
redis.call('XADD', KEYS[2], '*', 'event_id', ARGV[6], 'event_type', 'ORDER_QUANTITY_CHANGED',
  'order_id', ARGV[1], 'quantity', ARGV[3], 'remaining_quantity', tostring(newRemaining),
  'sequence_number', sequence, 'version', tostring(version))
return {1, sequence, version, newRemaining}
)";

const std::string modify_price_script = std::string(R"(
if redis.call('EXISTS', KEYS[1]) == 0 then return -1 end
)") + active_status + R"(
if redis.call('HGET', KEYS[1], 'trader_id') ~= ARGV[2] then return -3 end
if tonumber(redis.call('HGET', KEYS[1], 'version')) ~= tonumber(ARGV[6]) then return -4 end
local orderType = redis.call('HGET', KEYS[1], 'type')
if (orderType == 'LIMIT' or orderType == 'STOP_LIMIT') and ARGV[3] == '' then return -5 end
if (orderType == 'STOP' or orderType == 'STOP_LIMIT') and ARGV[4] == '' then return -6 end
local oldKey = redis.call('HGET', KEYS[1], 'book_key') or ''
local oldMember = redis.call('HGET', KEYS[1], 'book_member') or ''
if tonumber(ARGV[5]) <= tonumber(redis.call('HGET', KEYS[1], 'sequence_number')) then return -7 end
if oldKey ~= '' then redis.call('ZREM', oldKey, oldMember) end
if ARGV[8] ~= '' then redis.call('ZADD', ARGV[8], ARGV[9], ARGV[10]) end
local version = tonumber(ARGV[6]) + 1
redis.call('HSET', KEYS[1], 'limit_price_ticks', ARGV[3], 'stop_price_ticks', ARGV[4],
  'sequence_number', ARGV[5], 'book_member', ARGV[10], 'book_key', ARGV[8], 'version', tostring(version))
redis.call('XADD', KEYS[2], '*', 'event_id', ARGV[7], 'event_type', 'ORDER_PRICE_CHANGED',
  'order_id', ARGV[1], 'limit_price_ticks', ARGV[3], 'stop_price_ticks', ARGV[4],
  'sequence_number', ARGV[5], 'version', tostring(version))
return {1, version}
)";

const std::string activate_stop_script = std::string(R"(
if redis.call('EXISTS', KEYS[1]) == 0 then return -1 end
)") + active_status + R"(
if tonumber(redis.call('HGET', KEYS[1], 'version')) ~= tonumber(ARGV[2]) then return -3 end
if redis.call('HGET', KEYS[1], 'stop_activated') == '1' then return -4 end
local orderType = redis.call('HGET', KEYS[1], 'type')
if orderType ~= 'STOP' and orderType ~= 'STOP_LIMIT' then return -5 end
if tonumber(ARGV[5]) <= 0 or tonumber(ARGV[5]) - tonumber(ARGV[6]) > 1000 or tonumber(ARGV[6]) - tonumber(ARGV[5]) > tonumber(ARGV[7]) then return -6 end
local stopPrice = tonumber(redis.call('HGET', KEYS[1], 'stop_price_ticks'))
local tradePrice = tonumber(ARGV[3])
local side = redis.call('HGET', KEYS[1], 'side')
if (side == 'BUY' and tradePrice < stopPrice) or (side == 'SELL' and tradePrice > stopPrice) then return -7 end
local oldKey = redis.call('HGET', KEYS[1], 'book_key') or ''
local member = redis.call('HGET', KEYS[1], 'book_member')
if oldKey ~= '' then redis.call('ZREM', oldKey, member) end
if ARGV[8] ~= '' then redis.call('ZADD', ARGV[8], ARGV[9], member) end
local version = tonumber(ARGV[2]) + 1
redis.call('HSET', KEYS[1], 'stop_activated', '1', 'book_key', ARGV[8], 'version', tostring(version))
redis.call('XADD', KEYS[2], '*', 'event_id', ARGV[4], 'event_type', 'STOP_ACTIVATED',
  'order_id', ARGV[1], 'last_trade_price_ticks', ARGV[3], 'version', tostring(version))
return {1, version}
)";

const std::string fill_order_script = std::string(R"(
if redis.call('EXISTS', KEYS[1]) == 0 then return {-1} end
)") + active_status + R"(
if tonumber(redis.call('HGET', KEYS[1], 'version')) ~= tonumber(ARGV[2]) then return {-3} end
local marketTime = tonumber(ARGV[7])
local now = tonumber(ARGV[8])
if marketTime <= 0 or marketTime - now > 1000 or now - marketTime > tonumber(ARGV[9]) then return {-4} end
local side = redis.call('HGET', KEYS[1], 'side')
local orderType = redis.call('HGET', KEYS[1], 'type')
local activated = redis.call('HGET', KEYS[1], 'stop_activated') == '1'
local bid = tonumber(ARGV[4])
local ask = tonumber(ARGV[5])
local executionPrice = 0
if orderType == 'LIMIT' or orderType == 'STOP_LIMIT' then
  if orderType == 'STOP_LIMIT' and not activated then return {-5} end
  local limitPrice = tonumber(redis.call('HGET', KEYS[1], 'limit_price_ticks'))
  if side == 'BUY' then
    if ask <= 0 or ask > limitPrice then return {-5} end
    executionPrice = ask
  else
    if bid <= 0 or bid < limitPrice then return {-5} end
    executionPrice = bid
  end
elseif orderType == 'MARKET' or (orderType == 'STOP' and activated) then
  executionPrice = side == 'BUY' and ask or bid
  if executionPrice <= 0 then return {-5} end
else
  return {-5}
end
local remaining = tonumber(redis.call('HGET', KEYS[1], 'remaining_quantity'))
local requested = tonumber(ARGV[3])
if requested <= 0 then return {-7} end
if redis.call('SET', KEYS[3], ARGV[11], 'NX') == false then return {-6} end
local filled = math.min(requested, remaining)
remaining = remaining - filled
local version = tonumber(ARGV[2]) + 1
local status = remaining == 0 and 'FILLED' or 'PARTIALLY_FILLED'
local trader = redis.call('HGET', KEYS[1], 'trader_id')
local instrument = redis.call('HGET', KEYS[1], 'instrument_id')
if remaining == 0 then
  local bookKey = redis.call('HGET', KEYS[1], 'book_key') or ''
  local member = redis.call('HGET', KEYS[1], 'book_member') or ''
  if bookKey ~= '' then redis.call('ZREM', bookKey, member) end
  redis.call('SREM', 'trader:' .. trader .. ':open_orders', ARGV[1])
  redis.call('SET', 'terminal:' .. ARGV[1], 'FILLED')
  redis.call('DEL', KEYS[1])
else
  redis.call('HSET', KEYS[1], 'remaining_quantity', tostring(remaining), 'status', status, 'version', tostring(version))
end
redis.call('XADD', KEYS[2], '*', 'event_id', ARGV[10], 'event_type', status,
  'order_id', ARGV[1], 'trader_id', trader, 'instrument_id', instrument,
  'filled_quantity', tostring(filled), 'remaining_quantity', tostring(remaining), 'version', tostring(version))
redis.call('XADD', KEYS[2], '*', 'event_id', ARGV[10] .. ':execution', 'event_type', 'EXECUTION',
  'execution_id', ARGV[11], 'order_id', ARGV[1], 'trader_id', trader, 'instrument_id', instrument,
  'filled_quantity', tostring(filled), 'remaining_quantity', tostring(remaining),
  'execution_price_ticks', tostring(executionPrice), 'market_bid_ticks', ARGV[4],
  'market_ask_ticks', ARGV[5], 'price_source', ARGV[12], 'market_timestamp', ARGV[13],
  'execution_timestamp', ARGV[14])
return {1, filled, remaining, executionPrice, version, status}
)";

const std::string remove_order_script = R"(
if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end
local bookKey = redis.call('HGET', KEYS[1], 'book_key') or ''
local member = redis.call('HGET', KEYS[1], 'book_member') or ''
local trader = redis.call('HGET', KEYS[1], 'trader_id') or ''
if bookKey ~= '' then redis.call('ZREM', bookKey, member) end
if trader ~= '' then redis.call('SREM', 'trader:' .. trader .. ':open_orders', ARGV[1]) end
redis.call('DEL', KEYS[1])
return 1
)";

bool active(simtrade::order::OrderStatus status) {
  using simtrade::order::OrderStatus;
  return status == OrderStatus::Pending || status == OrderStatus::Open || status == OrderStatus::PartiallyFilled;
}

std::string value_or_empty(const std::optional<simtrade::order::Price>& value) {
  return value ? std::to_string(*value) : "";
}

std::map<std::string, std::string> hash_values(const redisReply* reply) {
  if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY || reply->elements % 2 != 0) {
    throw std::runtime_error("invalid Redis hash response");
  }
  std::map<std::string, std::string> values;
  for (std::size_t index = 0; index < reply->elements; index += 2) {
    const auto* key = reply->element[index];
    const auto* value = reply->element[index + 1];
    if (key == nullptr || value == nullptr || key->str == nullptr || value->str == nullptr) continue;
    values.emplace(std::string(key->str, static_cast<std::size_t>(key->len)),
                   std::string(value->str, static_cast<std::size_t>(value->len)));
  }
  return values;
}

simtrade::order::OrderId member_order_id(const std::string& member) {
  const auto separator = member.rfind(':');
  if (separator == std::string::npos || separator + 1 == member.size()) throw std::runtime_error("invalid order book member");
  return std::stoull(member.substr(separator + 1));
}

}  // namespace

namespace simtrade::order {

struct RedisOrderStore::Reply {
  explicit Reply(redisReply* reply) : value(reply) {}
  ~Reply() { if (value != nullptr) freeReplyObject(value); }
  redisReply* value;
};

RedisOrderStore::RedisOrderStore(std::string redis_url, std::uint32_t maximum_price_age_ms)
    : redis_url_(std::move(redis_url)), maximum_price_age_ms_(maximum_price_age_ms) {
  const std::regex pattern(R"(^redis://(?:([^@/]*)@)?([^:/]+)(?::([0-9]+))?(?:/([0-9]+))?$)");
  std::smatch match;
  if (!std::regex_match(redis_url_, match, pattern)) throw std::invalid_argument("invalid Redis URL");
  host_ = match[2].str();
  port_ = match[3].matched ? std::stoi(match[3].str()) : 6379;
  database_ = match[4].matched ? std::stoi(match[4].str()) : 0;
  if (match[1].matched) {
    const auto credentials = match[1].str();
    const auto separator = credentials.find(':');
    if (separator == std::string::npos) {
      password_ = credentials;
    } else {
      username_ = credentials.substr(0, separator);
      password_ = credentials.substr(separator + 1);
    }
  }
  std::scoped_lock lock(mutex_);
  static_cast<void>(connect_locked());
}

RedisOrderStore::~RedisOrderStore() {
  std::scoped_lock lock(mutex_);
  disconnect_locked();
}

bool RedisOrderStore::connect_locked() {
  disconnect_locked();
  const timeval timeout{1, 500000};
  context_ = redisConnectWithTimeout(host_.c_str(), port_, timeout);
  if (context_ == nullptr || context_->err != 0) {
    error_ = context_ == nullptr ? "Redis connection allocation failed" : context_->errstr;
    disconnect_locked();
    return false;
  }
  if (!password_.empty()) {
    auto* auth = username_.empty()
        ? static_cast<redisReply*>(redisCommand(context_, "AUTH %b", password_.data(), password_.size()))
        : static_cast<redisReply*>(redisCommand(context_, "AUTH %b %b", username_.data(), username_.size(),
                                                password_.data(), password_.size()));
    const bool success = auth != nullptr && auth->type != REDIS_REPLY_ERROR;
    if (!success) error_ = auth != nullptr && auth->str != nullptr ? auth->str : "Redis authentication failed";
    if (auth != nullptr) freeReplyObject(auth);
    if (!success) { disconnect_locked(); return false; }
  }
  if (database_ != 0) {
    auto* select = static_cast<redisReply*>(redisCommand(context_, "SELECT %d", database_));
    const bool success = select != nullptr && select->type != REDIS_REPLY_ERROR;
    if (!success) error_ = select != nullptr && select->str != nullptr ? select->str : "Redis database selection failed";
    if (select != nullptr) freeReplyObject(select);
    if (!success) { disconnect_locked(); return false; }
  }
  error_.clear();
  return true;
}

void RedisOrderStore::disconnect_locked() {
  if (context_ != nullptr) redisFree(context_);
  context_ = nullptr;
}

std::unique_ptr<RedisOrderStore::Reply> RedisOrderStore::command(const std::vector<std::string>& arguments) {
  std::scoped_lock lock(mutex_);
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (context_ == nullptr && !connect_locked()) continue;
    std::vector<const char*> values;
    std::vector<std::size_t> lengths;
    values.reserve(arguments.size());
    lengths.reserve(arguments.size());
    for (const auto& argument : arguments) { values.push_back(argument.data()); lengths.push_back(argument.size()); }
    auto* raw = static_cast<redisReply*>(redisCommandArgv(context_, static_cast<int>(values.size()), values.data(), lengths.data()));
    if (raw != nullptr) {
      if (raw->type == REDIS_REPLY_ERROR) {
        const auto message = raw->str == nullptr ? "Redis command failed" : std::string(raw->str, static_cast<std::size_t>(raw->len));
        freeReplyObject(raw);
        throw std::runtime_error(message);
      }
      return std::make_unique<Reply>(raw);
    }
    error_ = context_ != nullptr ? context_->errstr : "Redis command returned no response";
    disconnect_locked();
  }
  throw std::runtime_error(error_.empty() ? "Redis unavailable" : error_);
}

std::vector<std::unique_ptr<RedisOrderStore::Reply>> RedisOrderStore::pipeline(
    const std::vector<std::vector<std::string>>& commands) {
  std::scoped_lock lock(mutex_);
  if (context_ == nullptr && !connect_locked()) throw std::runtime_error(error_);
  for (const auto& arguments : commands) {
    std::vector<const char*> values;
    std::vector<std::size_t> lengths;
    for (const auto& argument : arguments) { values.push_back(argument.data()); lengths.push_back(argument.size()); }
    if (redisAppendCommandArgv(context_, static_cast<int>(values.size()), values.data(), lengths.data()) != REDIS_OK) {
      error_ = context_->errstr;
      disconnect_locked();
      throw std::runtime_error(error_);
    }
  }
  std::vector<std::unique_ptr<Reply>> replies;
  replies.reserve(commands.size());
  for (std::size_t index = 0; index < commands.size(); ++index) {
    void* raw = nullptr;
    if (redisGetReply(context_, &raw) != REDIS_OK || raw == nullptr) {
      error_ = context_ != nullptr ? context_->errstr : "Redis pipeline failed";
      disconnect_locked();
      throw std::runtime_error(error_);
    }
    auto reply = std::make_unique<Reply>(static_cast<redisReply*>(raw));
    if (reply->value->type == REDIS_REPLY_ERROR) {
      throw std::runtime_error(reply->value->str == nullptr ? "Redis pipeline command failed" : reply->value->str);
    }
    replies.push_back(std::move(reply));
  }
  return replies;
}

bool RedisOrderStore::healthy() {
  try {
    const auto reply = command({"PING"});
    return reply->value->type == REDIS_REPLY_STATUS && reply->value->str != nullptr &&
           std::string(reply->value->str, static_cast<std::size_t>(reply->value->len)) == "PONG";
  } catch (const std::exception&) {
    return false;
  }
}

std::string RedisOrderStore::status() const {
  std::scoped_lock lock(mutex_);
  return context_ != nullptr ? "ok" : (error_.empty() ? "unavailable" : error_);
}

void RedisOrderStore::reset_connection() {
  std::scoped_lock lock(mutex_);
  disconnect_locked();
}

std::string RedisOrderStore::book_member(std::uint64_t sequence_number, OrderId order_id) {
  std::ostringstream value;
  value << std::setw(20) << std::setfill('0') << sequence_number << ':' << order_id;
  return value.str();
}

std::string RedisOrderStore::limit_book_key(InstrumentId instrument_id, Side side) {
  return "book:" + std::to_string(instrument_id) + ":limit:" + (side == Side::Buy ? "buy" : "sell");
}

std::string RedisOrderStore::stop_book_key(InstrumentId instrument_id, Side side) {
  return "book:" + std::to_string(instrument_id) + ":stop:" + (side == Side::Buy ? "buy" : "sell");
}

std::string RedisOrderStore::market_book_key(InstrumentId instrument_id, Side side) {
  return "book:" + std::to_string(instrument_id) + ":market:" + (side == Side::Buy ? "buy" : "sell");
}

std::vector<std::string> RedisOrderStore::add_order_command(const Order& order) const {
  validate(order);
  if (!active(order.status)) throw std::invalid_argument("only active orders can be indexed");
  const auto member = book_member(order.sequenceNumber, order.id);
  std::string book_key;
  std::int64_t score = static_cast<std::int64_t>(order.sequenceNumber);
  if (order.type == OrderType::Market || (order.type == OrderType::Stop && order.stopActivated)) {
    book_key = market_book_key(order.instrumentId, order.side);
  } else if ((order.type == OrderType::Stop || order.type == OrderType::StopLimit) && !order.stopActivated) {
    book_key = stop_book_key(order.instrumentId, order.side);
    score = order.side == Side::Buy ? *order.stopPrice : -*order.stopPrice;
  } else {
    book_key = limit_book_key(order.instrumentId, order.side);
    score = order.side == Side::Buy ? -*order.limitPrice : *order.limitPrice;
  }
  return {"EVAL", add_order_script, "4", "order:" + std::to_string(order.id),
          "trader:" + std::to_string(order.traderId) + ":open_orders", book_key, "stream:order_events",
          std::to_string(order.id), std::to_string(order.traderId), std::to_string(order.instrumentId),
          std::string(to_string(order.side)), std::string(to_string(order.type)), std::string(to_string(order.status)),
          value_or_empty(order.limitPrice), value_or_empty(order.stopPrice), std::to_string(order.quantity),
          std::to_string(order.remainingQuantity), std::to_string(order.sequenceNumber), member,
          std::to_string(order.version), std::to_string(score), "recovery:add:" + std::to_string(order.id), book_key,
          order.stopActivated ? "1" : "0"};
}

bool RedisOrderStore::add_order(const Order& order) {
  const auto reply = command(add_order_command(order));
  return reply->value->type == REDIS_REPLY_INTEGER && reply->value->integer >= 0;
}

bool RedisOrderStore::cancel_order(OrderId order_id, TraderId trader_id, std::uint32_t expected_version,
                                   const std::string& event_id) {
  const auto reply = command({"EVAL", cancel_order_script, "2", "order:" + std::to_string(order_id),
                              "stream:order_events", std::to_string(order_id), std::to_string(trader_id),
                              std::to_string(expected_version), event_id});
  return reply->value->type == REDIS_REPLY_INTEGER && reply->value->integer == 1;
}

bool RedisOrderStore::modify_quantity(OrderId order_id, TraderId trader_id, Quantity new_quantity,
                                      std::uint64_t new_sequence_number, std::uint32_t expected_version,
                                      const std::string& event_id) {
  const auto order = find_order(order_id);
  if (!order) return false;
  const auto member = book_member(new_sequence_number, order_id);
  const bool activated = order->stopActivated;
  std::int64_t score = static_cast<std::int64_t>(new_sequence_number);
  if (order->type == OrderType::Limit || (order->type == OrderType::StopLimit && activated)) {
    score = order->side == Side::Buy ? -*order->limitPrice : *order->limitPrice;
  } else if ((order->type == OrderType::Stop || order->type == OrderType::StopLimit) && !activated) {
    score = order->side == Side::Buy ? *order->stopPrice : -*order->stopPrice;
  }
  const auto reply = command({"EVAL", modify_quantity_script, "2", "order:" + std::to_string(order_id),
                              "stream:order_events", std::to_string(order_id), std::to_string(trader_id),
                              std::to_string(new_quantity), std::to_string(new_sequence_number),
                              std::to_string(expected_version), event_id, member, std::to_string(score)});
  return reply->value->type == REDIS_REPLY_ARRAY && reply->value->elements > 0 &&
         reply->value->element[0]->integer == 1;
}

bool RedisOrderStore::modify_price(OrderId order_id, TraderId trader_id,
                                   std::optional<Price> limit_price, std::optional<Price> stop_price,
                                   std::uint64_t new_sequence_number, std::uint32_t expected_version,
                                   const std::string& event_id) {
  const auto order = find_order(order_id);
  if (!order) return false;
  const bool activated = order->stopActivated;
  if (order->type == OrderType::Stop && activated) return false;
  const auto resolved_limit = limit_price ? limit_price : order->limitPrice;
  const auto resolved_stop = stop_price ? stop_price : order->stopPrice;
  std::string book_key;
  std::int64_t score = static_cast<std::int64_t>(new_sequence_number);
  if (order->type == OrderType::Market) {
    book_key = market_book_key(order->instrumentId, order->side);
  } else if ((order->type == OrderType::Stop || order->type == OrderType::StopLimit) && !activated) {
    if (!resolved_stop) return false;
    book_key = stop_book_key(order->instrumentId, order->side);
    score = order->side == Side::Buy ? *resolved_stop : -*resolved_stop;
  } else {
    if (!resolved_limit) return false;
    book_key = limit_book_key(order->instrumentId, order->side);
    score = order->side == Side::Buy ? -*resolved_limit : *resolved_limit;
  }
  const auto member = book_member(new_sequence_number, order_id);
  const auto reply = command({"EVAL", modify_price_script, "2", "order:" + std::to_string(order_id),
                              "stream:order_events", std::to_string(order_id), std::to_string(trader_id),
                              value_or_empty(resolved_limit), value_or_empty(resolved_stop),
                              std::to_string(new_sequence_number), std::to_string(expected_version), event_id,
                              book_key, std::to_string(score), member});
  return reply->value->type == REDIS_REPLY_ARRAY && reply->value->elements > 0 &&
         reply->value->element[0]->integer == 1;
}

bool RedisOrderStore::activate_stop(OrderId order_id, std::uint32_t expected_version,
                                    Price last_trade_price, std::int64_t market_timestamp_ms,
                                    std::int64_t now_ms, const std::string& event_id) {
  const auto order = find_order(order_id);
  if (!order || (order->type != OrderType::Stop && order->type != OrderType::StopLimit)) return false;
  const auto destination = order->type == OrderType::StopLimit
      ? limit_book_key(order->instrumentId, order->side)
      : market_book_key(order->instrumentId, order->side);
  const auto score = order->type == OrderType::StopLimit
      ? (order->side == Side::Buy ? -*order->limitPrice : *order->limitPrice)
      : static_cast<Price>(order->sequenceNumber);
  const auto reply = command({"EVAL", activate_stop_script, "2", "order:" + std::to_string(order_id),
                              "stream:order_events", std::to_string(order_id), std::to_string(expected_version),
                              std::to_string(last_trade_price), event_id, std::to_string(market_timestamp_ms),
                              std::to_string(now_ms), std::to_string(maximum_price_age_ms_), destination,
                              std::to_string(score)});
  return reply->value->type == REDIS_REPLY_ARRAY && reply->value->elements > 0 &&
         reply->value->element[0]->integer == 1;
}

FillResult RedisOrderStore::apply_fill(const FillRequest& request) {
  const auto reply = command({"EVAL", fill_order_script, "3", "order:" + std::to_string(request.orderId),
                              "stream:order_events", "execution:" + request.executionId,
                              std::to_string(request.orderId), std::to_string(request.expectedVersion),
                              std::to_string(request.quantity), std::to_string(request.market.bidPrice),
                              std::to_string(request.market.askPrice), std::to_string(request.market.lastTradePrice),
                              std::to_string(request.market.marketTimestampMs),
                              std::to_string(request.executionTimestampMs), std::to_string(maximum_price_age_ms_),
                              request.eventId, request.executionId, request.market.source,
                              request.market.marketTimestamp, std::to_string(request.executionTimestampMs)});
  FillResult result;
  if (reply->value->type != REDIS_REPLY_ARRAY || reply->value->elements == 0) return result;
  const auto code = reply->value->element[0]->integer;
  result.duplicate = code == -6;
  result.stale = code == -4;
  result.priceConditionFailed = code == -5;
  if (code != 1 || reply->value->elements < 6) return result;
  result.applied = true;
  result.filledQuantity = reply->value->element[1]->integer;
  result.remainingQuantity = reply->value->element[2]->integer;
  result.executionPrice = reply->value->element[3]->integer;
  result.version = static_cast<std::uint32_t>(reply->value->element[4]->integer);
  const auto* status = reply->value->element[5];
  result.status = order_status_from_string(std::string(status->str, static_cast<std::size_t>(status->len)));
  return result;
}

void RedisOrderStore::store_quote(const MarketSnapshot& snapshot) {
  static_cast<void>(command({"HSET", "quote:" + std::to_string(snapshot.instrumentId),
                             "bid_price_ticks", std::to_string(snapshot.bidPrice),
                             "ask_price_ticks", std::to_string(snapshot.askPrice),
                             "bid_size", std::to_string(snapshot.bidSize), "ask_size", std::to_string(snapshot.askSize),
                             "last_trade_price_ticks", std::to_string(snapshot.lastTradePrice), "source", snapshot.source,
                             "market_timestamp", snapshot.marketTimestamp,
                             "market_timestamp_ms", std::to_string(snapshot.marketTimestampMs),
                             "received_timestamp_ms", std::to_string(snapshot.receivedTimestampMs)}));
}

void RedisOrderStore::store_trade(const MarketSnapshot& snapshot) {
  static_cast<void>(command({"HSET", "trade:" + std::to_string(snapshot.instrumentId),
                             "bid_price_ticks", std::to_string(snapshot.bidPrice),
                             "ask_price_ticks", std::to_string(snapshot.askPrice),
                             "bid_size", std::to_string(snapshot.bidSize), "ask_size", std::to_string(snapshot.askSize),
                             "last_trade_price_ticks", std::to_string(snapshot.lastTradePrice), "source", snapshot.source,
                             "market_timestamp", snapshot.marketTimestamp,
                             "market_timestamp_ms", std::to_string(snapshot.marketTimestampMs),
                             "received_timestamp_ms", std::to_string(snapshot.receivedTimestampMs)}));
}

std::optional<MarketSnapshot> RedisOrderStore::latest_quote(InstrumentId instrument_id) {
  const auto reply = command({"HGETALL", "quote:" + std::to_string(instrument_id)});
  const auto values = hash_values(reply->value);
  if (values.empty()) return std::nullopt;
  return MarketSnapshot{instrument_id, std::stoll(values.at("bid_price_ticks")),
                        std::stoll(values.at("ask_price_ticks")), std::stoll(values.at("bid_size")),
                        std::stoll(values.at("ask_size")), std::stoll(values.at("last_trade_price_ticks")),
                        values.at("source"), values.at("market_timestamp"),
                        std::stoll(values.at("market_timestamp_ms")), std::stoll(values.at("received_timestamp_ms"))};
}

std::optional<MarketSnapshot> RedisOrderStore::latest_trade(InstrumentId instrument_id) {
  const auto reply = command({"HGETALL", "trade:" + std::to_string(instrument_id)});
  const auto values = hash_values(reply->value);
  if (values.empty()) return std::nullopt;
  return MarketSnapshot{instrument_id, std::stoll(values.at("bid_price_ticks")),
                        std::stoll(values.at("ask_price_ticks")), std::stoll(values.at("bid_size")),
                        std::stoll(values.at("ask_size")), std::stoll(values.at("last_trade_price_ticks")),
                        values.at("source"), values.at("market_timestamp"),
                        std::stoll(values.at("market_timestamp_ms")), std::stoll(values.at("received_timestamp_ms"))};
}

std::optional<Order> RedisOrderStore::find_order(OrderId order_id) {
  const auto reply = command({"HGETALL", "order:" + std::to_string(order_id)});
  const auto values = hash_values(reply->value);
  if (values.empty()) return std::nullopt;
  Order order;
  order.id = order_id;
  order.clientOrderId = "redis:" + std::to_string(order_id);
  order.traderId = std::stoull(values.at("trader_id"));
  order.instrumentId = static_cast<InstrumentId>(std::stoul(values.at("instrument_id")));
  order.side = side_from_string(values.at("side"));
  order.type = order_type_from_string(values.at("type"));
  order.status = order_status_from_string(values.at("status"));
  if (!values.at("limit_price_ticks").empty()) order.limitPrice = std::stoll(values.at("limit_price_ticks"));
  if (!values.at("stop_price_ticks").empty()) order.stopPrice = std::stoll(values.at("stop_price_ticks"));
  order.quantity = std::stoll(values.at("quantity"));
  order.remainingQuantity = std::stoll(values.at("remaining_quantity"));
  order.sequenceNumber = std::stoull(values.at("sequence_number"));
  order.version = static_cast<std::uint32_t>(std::stoul(values.at("version")));
  order.stopActivated = values.at("stop_activated") == "1";
  return order;
}

std::vector<OrderId> RedisOrderStore::indexed_orders(const std::string& key, std::size_t limit) {
  if (limit == 0) return {};
  const auto reply = command({"ZRANGE", key, "0", std::to_string(limit == 0 ? 0 : limit - 1)});
  if (reply->value->type != REDIS_REPLY_ARRAY) throw std::runtime_error("invalid Redis sorted-set response");
  std::vector<OrderId> orders;
  orders.reserve(reply->value->elements);
  for (std::size_t index = 0; index < reply->value->elements; ++index) {
    const auto* element = reply->value->element[index];
    orders.push_back(member_order_id(std::string(element->str, static_cast<std::size_t>(element->len))));
  }
  return orders;
}

std::vector<OrderId> RedisOrderStore::eligible_limit_orders(InstrumentId instrument_id, Side side,
                                                            Price market_price, std::size_t limit) {
  const auto maximum = side == Side::Buy ? -market_price : market_price;
  const auto reply = command({"ZRANGEBYSCORE", limit_book_key(instrument_id, side), "-inf",
                              std::to_string(maximum), "LIMIT", "0", std::to_string(limit)});
  if (reply->value->type != REDIS_REPLY_ARRAY) throw std::runtime_error("invalid Redis price-range response");
  std::vector<OrderId> orders;
  for (std::size_t index = 0; index < reply->value->elements; ++index) {
    const auto* element = reply->value->element[index];
    orders.push_back(member_order_id(std::string(element->str, static_cast<std::size_t>(element->len))));
  }
  return orders;
}

std::vector<OrderId> RedisOrderStore::eligible_stop_orders(InstrumentId instrument_id, Side side,
                                                           Price last_trade_price, std::size_t limit) {
  const auto maximum = side == Side::Buy ? last_trade_price : -last_trade_price;
  const auto reply = command({"ZRANGEBYSCORE", stop_book_key(instrument_id, side), "-inf",
                              std::to_string(maximum), "LIMIT", "0", std::to_string(limit)});
  if (reply->value->type != REDIS_REPLY_ARRAY) throw std::runtime_error("invalid Redis stop-range response");
  std::vector<OrderId> orders;
  for (std::size_t index = 0; index < reply->value->elements; ++index) {
    const auto* element = reply->value->element[index];
    orders.push_back(member_order_id(std::string(element->str, static_cast<std::size_t>(element->len))));
  }
  return orders;
}

std::vector<OrderId> RedisOrderStore::market_orders(InstrumentId instrument_id, Side side, std::size_t limit) {
  return indexed_orders(market_book_key(instrument_id, side), limit);
}

std::size_t RedisOrderStore::stream_length() {
  const auto reply = command({"XLEN", "stream:order_events"});
  if (reply->value->type != REDIS_REPLY_INTEGER) throw std::runtime_error("invalid Redis stream response");
  return static_cast<std::size_t>(reply->value->integer);
}

std::uint32_t RedisOrderStore::maximum_price_age_ms() const noexcept { return maximum_price_age_ms_; }

bool RedisOrderStore::remove_order_unchecked(OrderId order_id) {
  const auto reply = command({"EVAL", remove_order_script, "1", "order:" + std::to_string(order_id),
                              std::to_string(order_id)});
  return reply->value->type == REDIS_REPLY_INTEGER && reply->value->integer == 1;
}

RecoveryResult RedisOrderStore::recover(const std::vector<Order>& active_orders,
                                        const std::vector<Instrument>& active_instruments) {
  RecoveryResult result;
  std::set<InstrumentId> valid_instruments;
  for (const auto& instrument : active_instruments) if (instrument.active) valid_instruments.insert(instrument.id);
  std::map<OrderId, const Order*> valid_orders;
  for (const auto& order : active_orders) {
    try {
      validate(order);
      if (!active(order.status) || !valid_instruments.contains(order.instrumentId)) {
        ++result.invalid;
        static_cast<void>(remove_order_unchecked(order.id));
        continue;
      }
      const auto terminal = command({"EXISTS", "terminal:" + std::to_string(order.id)});
      if (terminal->value->type != REDIS_REPLY_INTEGER) throw std::runtime_error("invalid Redis terminal response");
      if (terminal->value->integer == 1) {
        ++result.invalid;
        static_cast<void>(remove_order_unchecked(order.id));
        continue;
      }
      valid_orders.emplace(order.id, &order);
    } catch (const std::exception&) {
      ++result.invalid;
      static_cast<void>(remove_order_unchecked(order.id));
    }
  }

  std::string cursor = "0";
  do {
    const auto reply = command({"SCAN", cursor, "MATCH", "order:*", "COUNT", "100"});
    if (reply->value->type != REDIS_REPLY_ARRAY || reply->value->elements != 2) throw std::runtime_error("invalid Redis scan response");
    cursor.assign(reply->value->element[0]->str, static_cast<std::size_t>(reply->value->element[0]->len));
    const auto* keys = reply->value->element[1];
    for (std::size_t index = 0; index < keys->elements; ++index) {
      const std::string key(keys->element[index]->str, static_cast<std::size_t>(keys->element[index]->len));
      const auto id = std::stoull(key.substr(6));
      if (!valid_orders.contains(id) && remove_order_unchecked(id)) ++result.removed;
    }
  } while (cursor != "0");

  std::vector<std::vector<std::string>> cleanup_commands;
  cursor = "0";
  do {
    const auto reply = command({"SCAN", cursor, "MATCH", "book:*", "COUNT", "100"});
    if (reply->value->type != REDIS_REPLY_ARRAY || reply->value->elements != 2) throw std::runtime_error("invalid Redis book scan response");
    cursor.assign(reply->value->element[0]->str, static_cast<std::size_t>(reply->value->element[0]->len));
    const auto* keys = reply->value->element[1];
    for (std::size_t index = 0; index < keys->elements; ++index) {
      const std::string key(keys->element[index]->str, static_cast<std::size_t>(keys->element[index]->len));
      const auto members = command({"ZRANGE", key, "0", "-1"});
      if (members->value->type != REDIS_REPLY_ARRAY) throw std::runtime_error("invalid Redis book response");
      for (std::size_t member_index = 0; member_index < members->value->elements; ++member_index) {
        const auto* element = members->value->element[member_index];
        const std::string member(element->str, static_cast<std::size_t>(element->len));
        if (!valid_orders.contains(member_order_id(member))) cleanup_commands.push_back({"ZREM", key, member});
      }
    }
  } while (cursor != "0");

  cursor = "0";
  do {
    const auto reply = command({"SCAN", cursor, "MATCH", "trader:*:open_orders", "COUNT", "100"});
    if (reply->value->type != REDIS_REPLY_ARRAY || reply->value->elements != 2) throw std::runtime_error("invalid Redis trader scan response");
    cursor.assign(reply->value->element[0]->str, static_cast<std::size_t>(reply->value->element[0]->len));
    const auto* keys = reply->value->element[1];
    for (std::size_t index = 0; index < keys->elements; ++index) {
      const std::string key(keys->element[index]->str, static_cast<std::size_t>(keys->element[index]->len));
      const auto members = command({"SMEMBERS", key});
      if (members->value->type != REDIS_REPLY_ARRAY) throw std::runtime_error("invalid Redis trader-set response");
      for (std::size_t member_index = 0; member_index < members->value->elements; ++member_index) {
        const auto* element = members->value->element[member_index];
        const std::string member(element->str, static_cast<std::size_t>(element->len));
        if (!valid_orders.contains(std::stoull(member))) cleanup_commands.push_back({"SREM", key, member});
      }
    }
  } while (cursor != "0");
  if (!cleanup_commands.empty()) {
    const auto cleanup_replies = pipeline(cleanup_commands);
    for (const auto& reply : cleanup_replies) {
      if (reply->value->type == REDIS_REPLY_INTEGER) result.removed += static_cast<std::size_t>(reply->value->integer);
    }
  }

  std::vector<std::vector<std::string>> commands;
  commands.reserve(valid_orders.size());
  for (const auto& [id, order] : valid_orders) {
    static_cast<void>(id);
    commands.push_back(add_order_command(*order));
  }
  if (!commands.empty()) {
    const auto replies = pipeline(commands);
    for (const auto& reply : replies) {
      if (reply->value->type != REDIS_REPLY_INTEGER || reply->value->integer < -2) {
        throw std::runtime_error("active-order recovery failed");
      }
      ++result.rebuilt;
    }
  }
  return result;
}

}  // namespace simtrade::order
