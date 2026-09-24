#include "market/MarketDataRestClient.hpp"

#include "market/MarketDataCore.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <curl/curl.h>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace simtrade::market {

namespace {

constexpr auto kMinimumRequestInterval = std::chrono::milliseconds(100);
constexpr auto kSnapshotFreshness = std::chrono::seconds(30);
constexpr auto kHistoricalLookback = std::chrono::hours(24 * 8);
constexpr int kRegularSessionOpenMinute = 9 * 60 + 30;
constexpr int kRegularSessionCloseMinute = 16 * 60;

std::size_t append_response(char *data, std::size_t size, std::size_t count,
                            void *target) {
  const auto bytes = size * count;
  static_cast<std::string *>(target)->append(data, bytes);
  return bytes;
}

std::string join_symbols(const std::vector<std::string> &symbols) {
  std::ostringstream output;
  for (std::size_t index = 0; index < symbols.size(); ++index) {
    if (index > 0)
      output << ',';
    output << normalize_symbol(symbols[index]);
  }
  return output.str();
}

std::optional<std::chrono::system_clock::time_point>
parse_timestamp(const std::string &timestamp) {
  if (timestamp.size() < 19)
    return std::nullopt;
  std::tm parsed{};
  std::istringstream input(timestamp.substr(0, 19));
  input >> std::get_time(&parsed, "%Y-%m-%dT%H:%M:%S");
  if (input.fail())
    return std::nullopt;
#ifdef _WIN32
  const auto seconds = _mkgmtime(&parsed);
#else
  const auto seconds = timegm(&parsed);
#endif
  if (seconds < 0)
    return std::nullopt;
  return std::chrono::system_clock::from_time_t(seconds);
}

std::tm utc_parts(std::chrono::system_clock::time_point value) {
  const auto seconds = std::chrono::system_clock::to_time_t(value);
  std::tm result{};
#ifdef _WIN32
  gmtime_s(&result, &seconds);
#else
  gmtime_r(&seconds, &result);
#endif
  return result;
}

std::chrono::system_clock::time_point utc_time(int year, int month, int day,
                                               int hour) {
  std::tm value{};
  value.tm_year = year - 1900;
  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
#ifdef _WIN32
  const auto seconds = _mkgmtime(&value);
#else
  const auto seconds = timegm(&value);
#endif
  return std::chrono::system_clock::from_time_t(seconds);
}

int first_sunday(int year, int month) {
  const auto first = utc_parts(utc_time(year, month, 1, 0));
  return 1 + ((7 - first.tm_wday) % 7);
}

bool eastern_daylight_time(std::chrono::system_clock::time_point timestamp) {
  const auto utc = utc_parts(timestamp);
  const int year = utc.tm_year + 1900;
  const int second_sunday_in_march = first_sunday(year, 3) + 7;
  const int first_sunday_in_november = first_sunday(year, 11);
  const auto daylight_start = utc_time(year, 3, second_sunday_in_march, 7);
  const auto daylight_end = utc_time(year, 11, first_sunday_in_november, 6);
  return timestamp >= daylight_start && timestamp < daylight_end;
}

struct EasternTimestamp {
  std::string date;
  int minute_of_day;
};

std::optional<EasternTimestamp>
eastern_timestamp(const std::string &timestamp) {
  const auto parsed = parse_timestamp(timestamp);
  if (!parsed)
    return std::nullopt;
  const auto offset = eastern_daylight_time(*parsed) ? std::chrono::hours(-4)
                                                      : std::chrono::hours(-5);
  const auto local = utc_parts(*parsed + offset);
  std::ostringstream date;
  date << std::put_time(&local, "%Y-%m-%d");
  return EasternTimestamp{date.str(), local.tm_hour * 60 + local.tm_min};
}

std::string utc_date(std::chrono::system_clock::time_point value) {
  const auto parts = utc_parts(value);
  std::ostringstream output;
  output << std::put_time(&parts, "%Y-%m-%d");
  return output.str();
}

} // namespace

MarketDataRestClient::MarketDataRestClient(const config::Config &config)
    : config_(config) {}

nlohmann::json MarketDataRestClient::quote(const std::string &raw_symbol) {
  const auto symbol = normalize_symbol(raw_symbol);
  const auto snapshots = latest_snapshots({symbol});
  if (snapshots.empty())
    throw std::runtime_error("quote_unavailable");
  const auto &snapshot = snapshots.front();
  return {{"symbol", symbol},
          {"bid", snapshot.value("bidPrice", 0.0)},
          {"ask", snapshot.value("askPrice", 0.0)},
          {"last", snapshot.value("price", 0.0)},
          {"timestamp", snapshot.value("timestamp", "")},
          {"source", "alpaca_rest_snapshot"},
          {"live", false},
          {"stale", snapshot.value("stale", true)},
          {"feed", config_.alpaca_data_feed}};
}

std::vector<nlohmann::json> MarketDataRestClient::latest_snapshots(
    const std::vector<std::string> &raw_symbols) {
  if (raw_symbols.empty())
    return {};
  std::vector<std::string> symbols;
  symbols.reserve(raw_symbols.size());
  for (const auto &raw_symbol : raw_symbols) {
    const auto symbol = normalize_symbol(raw_symbol);
    if (!valid_symbol(symbol))
      throw std::runtime_error("invalid_symbol");
    symbols.push_back(symbol);
  }
  std::sort(symbols.begin(), symbols.end());
  symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());

  auto base_url = config_.alpaca_data_rest_url;
  while (!base_url.empty() && base_url.back() == '/')
    base_url.pop_back();
  const auto response = authenticated_get(
      base_url + "/v2/stocks/snapshots?symbols=" + join_symbols(symbols) +
      "&feed=" + config_.alpaca_data_feed);
  const auto &snapshots =
      response.contains("snapshots") ? response["snapshots"] : response;

  std::vector<nlohmann::json> result;
  for (const auto &symbol : symbols) {
    if (!snapshots.contains(symbol))
      continue;
    const auto &snapshot = snapshots[symbol];
    const auto quote = snapshot.value("latestQuote", nlohmann::json::object());
    const auto trade = snapshot.value("latestTrade", nlohmann::json::object());
    const auto bar = snapshot.value("minuteBar", nlohmann::json::object());
    const auto timestamp =
        quote.value("t", trade.value("t", bar.value("t", "")));
    const auto last = trade.value("p", bar.value("c", 0.0));
    auto bid = quote.value("bp", last);
    auto ask = quote.value("ap", last);
    if (bid <= 0.0)
      bid = last;
    if (ask <= 0.0)
      ask = last;
    result.push_back({{"type", "snapshot"},
                      {"symbol", symbol},
                      {"bidPrice", bid},
                      {"askPrice", ask},
                      {"price", last},
                      {"timestamp", timestamp},
                      {"source", "alpaca_rest_snapshot"},
                      {"live", false},
                      {"stale", !market_data_timestamp_is_fresh(
                                    timestamp, kSnapshotFreshness)},
                      {"feed", config_.alpaca_data_feed}});
  }
  return result;
}

nlohmann::json
MarketDataRestClient::historical_bars(const std::string &raw_symbol,
                                      std::size_t limit) {
  const auto symbol = normalize_symbol(raw_symbol);
  if (!valid_symbol(symbol))
    throw std::runtime_error("invalid_symbol");
  limit = std::clamp<std::size_t>(limit, 1, 10000);
  auto base_url = config_.alpaca_data_rest_url;
  while (!base_url.empty() && base_url.back() == '/')
    base_url.pop_back();
  const auto start = utc_date(std::chrono::system_clock::now() -
                              kHistoricalLookback);
  auto response =
      authenticated_get(base_url + "/v2/stocks/" + symbol +
                        "/bars?timeframe=1Min&limit=" + std::to_string(limit) +
                        "&start=" + start + "&sort=asc&adjustment=raw&feed=" +
                        config_.alpaca_data_feed);
  response["bars"] = latest_regular_session_bars(
      response.value("bars", nlohmann::json::array()));
  response["symbol"] = symbol;
  response["source"] = "alpaca_rest_historical";
  response["live"] = false;
  response["timeZone"] = "America/New_York";
  response["sessionOpen"] = "09:30";
  response["sessionClose"] = "16:00";
  response["expectedSessionMinutes"] = 390;
  if (!response["bars"].empty()) {
    const auto session = eastern_timestamp(
        response["bars"].front().value("t", ""));
    response["sessionDate"] = session ? session->date : "";
  } else {
    response["sessionDate"] = "";
  }
  return response;
}

std::size_t MarketDataRestClient::request_count() const noexcept {
  return request_count_.load();
}

std::size_t MarketDataRestClient::rate_limit_wait_count() const noexcept {
  return rate_limit_wait_count_.load();
}

nlohmann::json MarketDataRestClient::authenticated_get(const std::string &url) {
  if (config_.alpaca_api_key_id.empty() ||
      config_.alpaca_api_secret_key.empty()) {
    throw std::runtime_error("alpaca_not_configured");
  }
  wait_for_rate_limit_slot();
  ++request_count_;

  CURL *curl = curl_easy_init();
  if (curl == nullptr)
    throw std::runtime_error("market_data_client_unavailable");
  std::string response_body;
  curl_slist *headers = nullptr;
  headers = curl_slist_append(
      headers, ("APCA-API-KEY-ID: " + config_.alpaca_api_key_id).c_str());
  headers = curl_slist_append(
      headers,
      ("APCA-API-SECRET-KEY: " + config_.alpaca_api_secret_key).c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "simtrade-api/0.1");
  const auto request_result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (request_result != CURLE_OK)
    throw std::runtime_error("market_data_request_failed");
  if (status == 401 || status == 403)
    throw std::runtime_error("alpaca_credentials_or_feed_rejected");
  if (status == 429)
    throw std::runtime_error("alpaca_rate_limit_reached");
  if (status < 200 || status >= 300)
    throw std::runtime_error("alpaca_http_" + std::to_string(status));
  return nlohmann::json::parse(response_body);
}

void MarketDataRestClient::wait_for_rate_limit_slot() {
  std::chrono::steady_clock::time_point slot;
  {
    std::scoped_lock lock(rate_limit_mutex_);
    const auto now = std::chrono::steady_clock::now();
    slot = std::max(now, next_request_at_);
    next_request_at_ = slot + kMinimumRequestInterval;
    if (slot > now)
      ++rate_limit_wait_count_;
  }
  std::this_thread::sleep_until(slot);
}

bool market_data_timestamp_is_fresh(const std::string &timestamp,
                                    std::chrono::seconds maximum_age,
                                    std::chrono::system_clock::time_point now) {
  const auto parsed = parse_timestamp(timestamp);
  if (!parsed || *parsed > now + std::chrono::seconds(5))
    return false;
  return now - *parsed <= maximum_age;
}

nlohmann::json latest_regular_session_bars(const nlohmann::json &bars) {
  if (!bars.is_array())
    return nlohmann::json::array();

  std::string latest_session;
  std::map<std::string, nlohmann::json> selected;
  for (const auto &bar : bars) {
    if (!bar.is_object())
      continue;
    const auto timestamp = bar.value("t", "");
    const auto eastern = eastern_timestamp(timestamp);
    if (!eastern || eastern->minute_of_day < kRegularSessionOpenMinute ||
        eastern->minute_of_day >= kRegularSessionCloseMinute)
      continue;
    if (eastern->date > latest_session) {
      latest_session = eastern->date;
      selected.clear();
    }
    if (eastern->date == latest_session)
      selected.insert_or_assign(timestamp, bar);
  }

  nlohmann::json result = nlohmann::json::array();
  for (const auto &[timestamp, bar] : selected) {
    static_cast<void>(timestamp);
    result.push_back(bar);
  }
  return result;
}

} // namespace simtrade::market
