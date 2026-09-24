#pragma once

#include "market/MarketDataCore.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace simtrade::market {

class MarketDataHub {
 public:
  using ClientId = std::uint64_t;
  using Sender = std::function<void(const std::string&)>;
  using CacheReader = std::function<std::optional<std::string>(const std::string&)>;

  MarketDataHub(SymbolSubscriptionSink& upstream, CacheReader cache_reader);
  ClientId add_client(Sender sender);
  void remove_client(ClientId client_id);
  void handle_client_message(ClientId client_id, const std::string& message);
  void publish(const nlohmann::json& event);
  [[nodiscard]] std::size_t client_count() const;

 private:
  struct Client {
    Sender sender;
    std::set<std::string> symbols;
  };

  void send_cached(const Sender& sender, const std::string& symbol) const;
  void send_error(const Sender& sender, const std::string& code, const std::string& message) const;

  SymbolSubscriptionSink& upstream_;
  CacheReader cache_reader_;
  mutable std::mutex mutex_;
  ClientId next_client_id_{1};
  std::map<ClientId, Client> clients_;
  std::map<std::string, std::size_t> symbol_interest_;
};

}  // namespace simtrade::market
