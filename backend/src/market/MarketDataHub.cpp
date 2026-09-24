#include "market/MarketDataHub.hpp"

#include <array>
#include <vector>

namespace simtrade::market {

MarketDataHub::MarketDataHub(SymbolSubscriptionSink& upstream, CacheReader cache_reader)
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
      auto interest = symbol_interest_.find(symbol);
      if (interest == symbol_interest_.end()) continue;
      if (--interest->second == 0) {
        symbol_interest_.erase(interest);
        released.push_back(symbol);
      }
    }
    clients_.erase(client);
  }
  for (const auto& symbol : released) upstream_.unsubscribe(symbol);
}

void MarketDataHub::handle_client_message(ClientId client_id, const std::string& message) {
  nlohmann::json input;
  try {
    input = nlohmann::json::parse(message);
  } catch (const nlohmann::json::exception&) {
    std::scoped_lock lock(mutex_);
    const auto client = clients_.find(client_id);
    if (client != clients_.end()) send_error(client->second.sender, "invalid_json", "Message must be valid JSON.");
    return;
  }

  const auto action = input.value("action", "");
  if ((action != "subscribe" && action != "unsubscribe") ||
      !input.contains("symbols") || !input["symbols"].is_array()) {
    std::scoped_lock lock(mutex_);
    const auto client = clients_.find(client_id);
    if (client != clients_.end()) send_error(client->second.sender, "invalid_message", "Use subscribe or unsubscribe with a symbols array.");
    return;
  }

  std::vector<std::pair<Sender, std::string>> cached_deliveries;
  std::vector<std::string> released;
  Sender sender;
  nlohmann::json accepted = nlohmann::json::array();
  {
    std::scoped_lock lock(mutex_);
    const auto client = clients_.find(client_id);
    if (client == clients_.end()) return;
    sender = client->second.sender;
    for (const auto& raw : input["symbols"]) {
      if (!raw.is_string()) continue;
      const auto symbol = normalize_symbol(raw.get<std::string>());
      if (!valid_symbol(symbol)) {
        send_error(sender, "invalid_symbol", "Instrument symbol is invalid.");
        continue;
      }
      if (action == "subscribe") {
        if (client->second.symbols.contains(symbol)) {
          accepted.push_back(symbol);
          continue;
        }
        if (!symbol_interest_.contains(symbol)) {
          const auto result = upstream_.subscribe(symbol);
          if (result == SubscriptionResult::limit_reached) {
            send_error(sender, "symbol_limit_reached", "The 30-symbol live stream limit has been reached.");
            continue;
          }
          if (result == SubscriptionResult::invalid_symbol) {
            send_error(sender, "invalid_symbol", "Instrument symbol is invalid.");
            continue;
          }
        }
        client->second.symbols.insert(symbol);
        ++symbol_interest_[symbol];
        accepted.push_back(symbol);
        cached_deliveries.emplace_back(sender, symbol);
      } else if (client->second.symbols.erase(symbol) > 0) {
        auto interest = symbol_interest_.find(symbol);
        if (interest != symbol_interest_.end() && --interest->second == 0) {
          symbol_interest_.erase(interest);
          released.push_back(symbol);
        }
        accepted.push_back(symbol);
      }
    }
  }

  for (const auto& symbol : released) upstream_.unsubscribe(symbol);
  for (const auto& [target, symbol] : cached_deliveries) send_cached(target, symbol);
  if (sender) sender(nlohmann::json({{"type", "clientSubscription"}, {"action", action}, {"symbols", accepted}}).dump());
}

void MarketDataHub::publish(const nlohmann::json& event) {
  const auto symbol = normalize_symbol(event.value("symbol", ""));
  if (!valid_symbol(symbol)) return;
  std::vector<Sender> recipients;
  {
    std::scoped_lock lock(mutex_);
    for (const auto& [id, client] : clients_) {
      static_cast<void>(id);
      if (client.symbols.contains(symbol)) recipients.push_back(client.sender);
    }
  }
  const auto payload = event.dump();
  for (const auto& sender : recipients) sender(payload);
}

std::size_t MarketDataHub::client_count() const {
  std::scoped_lock lock(mutex_);
  return clients_.size();
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
