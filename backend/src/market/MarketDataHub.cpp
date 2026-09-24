#include "market/MarketDataHub.hpp"

#include <array>
#include <vector>

namespace simtrade::market {

MarketDataHub::MarketDataHub(ViewerSubscriptionSink& upstream, CacheReader cache_reader)
    : upstream_(upstream), cache_reader_(std::move(cache_reader)) {}

MarketDataHub::ClientId MarketDataHub::add_client(Sender sender) {
  std::scoped_lock lock(mutex_);
  const auto id = next_client_id_++;
  clients_.emplace(id, Client{std::move(sender), {}});
  return id;
}

void MarketDataHub::remove_client(ClientId client_id) {
  std::vector<std::string> released;
  {
    std::scoped_lock lock(mutex_);
    const auto client = clients_.find(client_id);
    if (client == clients_.end()) return;
    for (const auto& symbol : client->second.symbols) {
      auto viewers = symbol_clients_.find(symbol);
      if (viewers != symbol_clients_.end()) {
        viewers->second.erase(client_id);
        if (viewers->second.empty()) symbol_clients_.erase(viewers);
      }
      released.push_back(symbol);
    }
    clients_.erase(client);
  }
  for (const auto& symbol : released) upstream_.unwatch(symbol);
}

void MarketDataHub::handle_client_message(ClientId client_id, const std::string& message) {
  nlohmann::json input;
  try {
    input = nlohmann::json::parse(message);
  } catch (const nlohmann::json::exception&) {
    Sender sender;
    {
      std::scoped_lock lock(mutex_);
      const auto client = clients_.find(client_id);
      if (client != clients_.end()) sender = client->second.sender;
    }
    send_error(sender, "invalid_json", "Message must be valid JSON.");
    return;
  }

  const auto action = input.value("action", "");
  const bool watching = action == "watch" || action == "subscribe";
  const bool unwatching = action == "unwatch" || action == "unsubscribe";
  std::vector<nlohmann::json> raw_symbols;
  if ((action == "watch" || action == "unwatch") && input.contains("symbol") && input["symbol"].is_string()) {
    raw_symbols.push_back(input["symbol"]);
  } else if ((action == "subscribe" || action == "unsubscribe") &&
             input.contains("symbols") && input["symbols"].is_array()) {
    for (const auto& value : input["symbols"]) raw_symbols.push_back(value);
  } else {
    Sender sender;
    {
      std::scoped_lock lock(mutex_);
      const auto client = clients_.find(client_id);
      if (client != clients_.end()) sender = client->second.sender;
    }
    send_error(sender,
               "invalid_message",
               "Use watch/unwatch with a symbol, or subscribe/unsubscribe with a symbols array.");
    return;
  }
  if (!watching && !unwatching) return;

  Sender sender;
  nlohmann::json accepted = nlohmann::json::array();
  {
    std::scoped_lock lock(mutex_);
    const auto client = clients_.find(client_id);
    if (client == clients_.end()) return;
    sender = client->second.sender;
  }

  for (const auto& raw : raw_symbols) {
    if (!raw.is_string()) continue;
    const auto symbol = normalize_symbol(raw.get<std::string>());
    if (!valid_symbol(symbol)) {
      send_error(sender, "invalid_symbol", "Instrument symbol is invalid.");
      continue;
    }

    if (watching) {
      bool duplicate = false;
      {
        std::scoped_lock lock(mutex_);
        const auto client = clients_.find(client_id);
        duplicate = client != clients_.end() && client->second.symbols.contains(symbol);
      }
      if (duplicate) {
        upstream_.touch(symbol);
        accepted.push_back(symbol);
        continue;
      }

      const auto result = upstream_.watch(symbol);
      if (result == WatchResult::invalid_symbol) {
        send_error(sender, "invalid_symbol", "Instrument symbol is not available in the catalogue.");
        continue;
      }
      if (result == WatchResult::capacity_full) {
        if (sender) sender(nlohmann::json({{"type", "market_data_status"},
                                          {"symbol", symbol},
                                          {"status", "CAPACITY_FULL"},
                                          {"live", false},
                                          {"message", "All live market-data slots are currently in use."}}).dump());
        continue;
      }

      bool registered = false;
      {
        std::scoped_lock lock(mutex_);
        const auto client = clients_.find(client_id);
        if (client != clients_.end()) {
          registered = client->second.symbols.insert(symbol).second;
          if (registered) symbol_clients_[symbol].insert(client_id);
        }
      }
      if (!registered) {
        upstream_.unwatch(symbol);
        continue;
      }
      accepted.push_back(symbol);
      if (result == WatchResult::live) send_cached(sender, symbol);
      if (sender) sender(nlohmann::json({{"type", "market_data_status"},
                                        {"symbol", symbol},
                                        {"status", result == WatchResult::live ? "LIVE" : "PENDING"},
                                        {"live", result == WatchResult::live},
                                        {"message", result == WatchResult::live
                                                        ? "Live market data is available."
                                                        : "Live market data subscription is pending."}}).dump());
    } else {
      bool removed = false;
      {
        std::scoped_lock lock(mutex_);
        const auto client = clients_.find(client_id);
        if (client != clients_.end()) {
          removed = client->second.symbols.erase(symbol) > 0;
          if (removed) {
            auto viewers = symbol_clients_.find(symbol);
            if (viewers != symbol_clients_.end()) {
              viewers->second.erase(client_id);
              if (viewers->second.empty()) symbol_clients_.erase(viewers);
            }
          }
        }
      }
      if (removed) upstream_.unwatch(symbol);
      accepted.push_back(symbol);
    }
  }

  if (sender) sender(nlohmann::json({{"type", "clientSubscription"}, {"action", action}, {"symbols", accepted}}).dump());
}

void MarketDataHub::publish(const nlohmann::json& event) {
  const auto symbol = normalize_symbol(event.value("symbol", ""));
  if (!valid_symbol(symbol)) return;
  std::vector<Sender> recipients;
  {
    std::scoped_lock lock(mutex_);
    const auto viewers = symbol_clients_.find(symbol);
    if (viewers != symbol_clients_.end()) {
      for (const auto client_id : viewers->second) {
        const auto client = clients_.find(client_id);
        if (client != clients_.end()) recipients.push_back(client->second.sender);
      }
    }
  }
  const auto payload = event.dump();
  for (const auto& sender : recipients) sender(payload);
}

void MarketDataHub::publish_status(const std::string& raw_symbol,
                                   const std::string& status,
                                   bool live,
                                   const std::string& message) {
  const auto symbol = normalize_symbol(raw_symbol);
  publish({{"type", "market_data_status"},
           {"symbol", symbol},
           {"status", status},
           {"live", live},
           {"message", message}});
  if (!live) return;

  std::vector<Sender> recipients;
  {
    std::scoped_lock lock(mutex_);
    const auto viewers = symbol_clients_.find(symbol);
    if (viewers != symbol_clients_.end()) {
      for (const auto client_id : viewers->second) {
        const auto client = clients_.find(client_id);
        if (client != clients_.end()) recipients.push_back(client->second.sender);
      }
    }
  }
  for (const auto& recipient : recipients) send_cached(recipient, symbol);
}

std::size_t MarketDataHub::client_count() const {
  std::scoped_lock lock(mutex_);
  return clients_.size();
}

std::size_t MarketDataHub::viewer_count(const std::string& raw_symbol) const {
  const auto symbol = normalize_symbol(raw_symbol);
  std::scoped_lock lock(mutex_);
  const auto viewers = symbol_clients_.find(symbol);
  return viewers == symbol_clients_.end() ? 0 : viewers->second.size();
}

void MarketDataHub::send_cached(const Sender& sender, const std::string& symbol) const {
  if (!cache_reader_) return;
  const std::array<std::string, 4> keys{
      "market:quote:" + symbol,
      "market:trade:" + symbol,
      "market:bar:1m:" + symbol,
      "market:status:" + symbol,
  };
  for (const auto& key : keys) {
    const auto cached = cache_reader_(key);
    if (cached) sender(*cached);
  }
}

void MarketDataHub::send_error(const Sender& sender,
                               const std::string& code,
                               const std::string& message) const {
  if (sender) sender(nlohmann::json({{"type", "error"}, {"error", code}, {"message", message}}).dump());
}

}  // namespace simtrade::market
