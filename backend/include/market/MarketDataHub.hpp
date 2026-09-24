#pragma once

#include "market/SubscriptionManager.hpp"

#include <cstdint>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace simtrade::market {

class MarketDataHub {
 public:
  using ClientId = std::uint64_t;
  using Sender = std::function<void(const std::string&)>;
  using CacheReader = std::function<std::optional<std::string>(const std::string&)>;
  using FallbackFetcher = std::function<std::vector<nlohmann::json>(const std::string&)>;

  MarketDataHub(ViewerSubscriptionSink& upstream,
                CacheReader cache_reader,
                FallbackFetcher fallback_fetcher = {});
  ClientId add_client(Sender sender);
  void remove_client(ClientId client_id);
  void handle_client_message(ClientId client_id, const std::string& message);
  void publish(const nlohmann::json& event);
  void publish_status(const std::string& symbol,
                      const std::string& status,
                      bool live,
                      std::optional<std::size_t> queue_position,
                      const std::string& message);
  [[nodiscard]] std::size_t client_count() const;
  [[nodiscard]] std::size_t viewer_count(const std::string& symbol) const;
  [[nodiscard]] std::size_t stale_data_event_count() const noexcept;
  [[nodiscard]] std::size_t malformed_message_count() const noexcept;
  [[nodiscard]] std::size_t cache_failure_count() const noexcept;

 private:
  struct Client {
    Sender sender;
    std::set<std::string> symbols;
  };

  void send_fallback(const Sender& sender, const std::string& symbol) const;
  void send_cached(const Sender& sender, const std::string& symbol) const;
  void send_error(const Sender& sender, const std::string& code, const std::string& message) const;

  ViewerSubscriptionSink& upstream_;
  CacheReader cache_reader_;
  FallbackFetcher fallback_fetcher_;
  mutable std::mutex mutex_;
  ClientId next_client_id_{1};
  std::map<ClientId, Client> clients_;
  std::map<std::string, std::set<ClientId>> symbol_clients_;
  mutable std::atomic<std::size_t> stale_data_event_count_{0};
  mutable std::atomic<std::size_t> malformed_message_count_{0};
  mutable std::atomic<std::size_t> cache_failure_count_{0};
};

}  // namespace simtrade::market
