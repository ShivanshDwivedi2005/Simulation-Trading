#include "market/InstrumentCatalogue.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <stdexcept>

namespace {

constexpr const char* catalogue_cache_key = "market:instruments:catalogue";

std::size_t append_response(char* data, std::size_t size, std::size_t count, void* target) {
  const auto bytes = size * count;
  static_cast<std::string*>(target)->append(data, bytes);
  return bytes;
}

nlohmann::json fetch_assets(const simtrade::config::Config& config) {
  if (config.alpaca_api_key_id.empty() || config.alpaca_api_secret_key.empty()) {
    throw std::runtime_error("alpaca_not_configured");
  }
  auto base_url = config.alpaca_trading_rest_url;
  while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
  const auto url = base_url.ends_with("/v2")
                       ? base_url + "/assets?status=active&asset_class=us_equity"
                       : base_url + "/v2/assets?status=active&asset_class=us_equity";

  CURL* curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("asset_catalogue_client_unavailable");
  std::string response_body;
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, ("APCA-API-KEY-ID: " + config.alpaca_api_key_id).c_str());
  headers = curl_slist_append(headers, ("APCA-API-SECRET-KEY: " + config.alpaca_api_secret_key).c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "simtrade-api/0.1");
  const auto result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK) throw std::runtime_error("asset_catalogue_request_failed");
  if (status == 401 || status == 403) throw std::runtime_error("alpaca_asset_credentials_rejected");
  if (status < 200 || status >= 300) throw std::runtime_error("alpaca_assets_http_" + std::to_string(status));
  return nlohmann::json::parse(response_body);
}

}  // namespace

namespace simtrade::market {

InstrumentCatalogue::InstrumentCatalogue(const config::Config& config, const cache::RedisClient& redis)
    : config_(config), redis_(redis) {}

InstrumentCatalogue::~InstrumentCatalogue() { stop(); }

void InstrumentCatalogue::start() {
  if (running_.exchange(true)) return;
  try {
    if (!load_cached_catalogue()) synchronize();
  } catch (...) {
    running_ = false;
    throw;
  }
  worker_ = std::thread([this] { run(); });
}

void InstrumentCatalogue::stop() {
  if (!running_.exchange(false)) return;
  condition_.notify_all();
  if (worker_.joinable()) worker_.join();
}

InstrumentSearchPage InstrumentCatalogue::search(const std::string& query,
                                                 std::size_t page,
                                                 std::size_t limit) const {
  std::scoped_lock lock(mutex_);
  return search_instruments(instruments_, query, page, limit);
}

bool InstrumentCatalogue::ready() const noexcept {
  std::scoped_lock lock(mutex_);
  return !instruments_.empty();
}

bool InstrumentCatalogue::contains(const std::string& symbol) const {
  const auto normalized = normalize_symbol(symbol);
  std::scoped_lock lock(mutex_);
  const auto found = std::lower_bound(instruments_.begin(), instruments_.end(), normalized,
                                      [](const Instrument& instrument, const std::string& value) {
                                        return instrument.symbol < value;
                                      });
  return found != instruments_.end() && found->symbol == normalized;
}

std::size_t InstrumentCatalogue::size() const {
  std::scoped_lock lock(mutex_);
  return instruments_.size();
}

std::string InstrumentCatalogue::last_synced_at() const {
  std::scoped_lock lock(mutex_);
  return last_synced_at_;
}

std::string InstrumentCatalogue::last_error() const {
  std::scoped_lock lock(mutex_);
  return last_error_;
}

bool InstrumentCatalogue::load_cached_catalogue() {
  const auto cached = redis_.get(catalogue_cache_key);
  if (!cached) return false;
  try {
    const auto document = nlohmann::json::parse(*cached);
    const auto instruments = parse_alpaca_assets(document.value("assets", nlohmann::json::array()));
    if (instruments.empty()) return false;
    std::scoped_lock lock(mutex_);
    instruments_ = instruments;
    last_synced_at_ = document.value("cachedAt", "");
    return true;
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

void InstrumentCatalogue::synchronize() {
  const auto raw_assets = fetch_assets(config_);
  auto instruments = parse_alpaca_assets(raw_assets);
  if (instruments.empty()) throw std::runtime_error("alpaca_asset_catalogue_empty");

  const auto now = std::chrono::system_clock::now();
  const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  const auto synced_at = std::to_string(epoch);
  nlohmann::json cache_document{{"cachedAt", synced_at}, {"assets", raw_assets}};
  static_cast<void>(redis_.set(catalogue_cache_key, cache_document.dump()));

  std::scoped_lock lock(mutex_);
  instruments_ = std::move(instruments);
  last_synced_at_ = synced_at;
  last_error_.clear();
}

void InstrumentCatalogue::run() {
  while (running_) {
    std::unique_lock lock(mutex_);
    const bool stopped = condition_.wait_for(lock,
                                             std::chrono::hours(config_.alpaca_asset_sync_interval_hours),
                                             [this] { return !running_; });
    lock.unlock();
    if (stopped) break;
    try {
      synchronize();
    } catch (const std::exception& exception) {
      std::scoped_lock error_lock(mutex_);
      last_error_ = exception.what();
    }
  }
}

}  // namespace simtrade::market
