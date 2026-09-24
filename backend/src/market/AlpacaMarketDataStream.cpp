#include "market/AlpacaMarketDataStream.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/rfc2818_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

struct WebSocketUrl {
  std::string host;
  std::string port;
  std::string target;
};

WebSocketUrl parse_websocket_url(const std::string& value) {
  constexpr std::string_view prefix = "wss://";
  if (!value.starts_with(prefix)) {
    throw std::runtime_error("alpaca_websocket_url_must_use_wss");
  }
  const auto authority_start = prefix.size();
  const auto path_start = value.find('/', authority_start);
  const auto authority = value.substr(authority_start, path_start - authority_start);
  if (authority.empty()) throw std::runtime_error("alpaca_websocket_url_missing_host");

  const auto port_separator = authority.rfind(':');
  WebSocketUrl result;
  if (port_separator != std::string::npos) {
    result.host = authority.substr(0, port_separator);
    result.port = authority.substr(port_separator + 1);
  } else {
    result.host = authority;
    result.port = "443";
  }
  result.target = path_start == std::string::npos ? "/" : value.substr(path_start);
  if (result.host.empty() || result.port.empty()) {
    throw std::runtime_error("alpaca_websocket_url_invalid_authority");
  }
  return result;
}

std::string current_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
         << milliseconds << 'Z';
  return output.str();
}

std::set<std::string> difference(const std::set<std::string>& left, const std::set<std::string>& right) {
  std::set<std::string> result;
  std::set_difference(left.begin(), left.end(), right.begin(), right.end(), std::inserter(result, result.end()));
  return result;
}

class AlpacaWebSocketSession final : public std::enable_shared_from_this<AlpacaWebSocketSession> {
 public:
  using MessageHandler = std::function<void(const std::string&)>;
  using ConnectionHandler = std::function<void(bool)>;

  AlpacaWebSocketSession(asio::io_context& io,
                         ssl::context& ssl_context,
                         WebSocketUrl url,
                         std::string authentication,
                         simtrade::market::SubscriptionRegistry& subscriptions,
                         const std::atomic<bool>& running,
                         MessageHandler message_handler,
                         ConnectionHandler connection_handler)
      : io_(io),
        resolver_(asio::make_strand(io)),
        websocket_(asio::make_strand(io), ssl_context),
        timer_(asio::make_strand(io)),
        url_(std::move(url)),
        authentication_(std::move(authentication)),
        subscriptions_(subscriptions),
        running_(running),
        message_handler_(std::move(message_handler)),
        connection_handler_(std::move(connection_handler)) {}

  void start() {
    if (!SSL_set_tlsext_host_name(websocket_.next_layer().native_handle(), url_.host.c_str())) {
      fail("tls_sni", beast::error_code(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()));
      return;
    }
    websocket_.next_layer().set_verify_callback(ssl::rfc2818_verification(url_.host));
    resolver_.async_resolve(url_.host,
                            url_.port,
                            beast::bind_front_handler(&AlpacaWebSocketSession::on_resolve, shared_from_this()));
  }

  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  void on_resolve(beast::error_code error, const tcp::resolver::results_type& results) {
    if (error) return fail("resolve", error);
    beast::get_lowest_layer(websocket_).expires_after(std::chrono::seconds(10));
    beast::get_lowest_layer(websocket_).async_connect(
        results, beast::bind_front_handler(&AlpacaWebSocketSession::on_connect, shared_from_this()));
  }

  void on_connect(beast::error_code error, const tcp::resolver::results_type::endpoint_type&) {
    if (error) return fail("connect", error);
    beast::get_lowest_layer(websocket_).expires_after(std::chrono::seconds(10));
    websocket_.next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&AlpacaWebSocketSession::on_tls_handshake, shared_from_this()));
  }

  void on_tls_handshake(beast::error_code error) {
    if (error) return fail("tls_handshake", error);
    beast::get_lowest_layer(websocket_).expires_never();
    auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::client);
    timeout.handshake_timeout = std::chrono::seconds(15);
    timeout.idle_timeout = std::chrono::seconds(120);
    timeout.keep_alive_pings = true;
    websocket_.set_option(timeout);
    websocket_.set_option(websocket::stream_base::decorator([](beast::http::request_header<>& request) {
      request.set(beast::http::field::user_agent, "simtrade-api/0.1");
    }));
    websocket_.async_handshake(url_.host,
                               url_.target,
                               beast::bind_front_handler(&AlpacaWebSocketSession::on_websocket_handshake,
                                                         shared_from_this()));
  }

  void on_websocket_handshake(beast::error_code error) {
    if (error) return fail("websocket_handshake", error);
    last_receive_ = std::chrono::steady_clock::now();
    queue_write(authentication_);
    read_next();
    schedule_tick();
  }

  void read_next() {
    websocket_.async_read(buffer_,
                          beast::bind_front_handler(&AlpacaWebSocketSession::on_read, shared_from_this()));
  }

  void on_read(beast::error_code error, std::size_t) {
    if (error) {
      if (closing_ && (error == websocket::error::closed || error == asio::error::operation_aborted)) return;
      return fail("read", error);
    }
    last_receive_ = std::chrono::steady_clock::now();
    const auto message = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());
    try {
      const auto document = nlohmann::json::parse(message);
      const auto values = document.is_array() ? document : nlohmann::json::array({document});
      for (const auto& value : values) {
        const auto type = value.value("T", "");
        const auto detail = value.value("msg", "");
        if (type == "error") {
          error_ = "alpaca_websocket_" + (detail.empty() ? std::string("error") : detail);
          close_socket();
          return;
        }
        if (type == "success" && detail == "authenticated") {
          authenticated_ = true;
          connection_handler_(true);
        }
        if (type == "subscription") subscriptions_.confirm(value);
      }
      message_handler_(message);
    } catch (const std::exception& exception) {
      error_ = std::string("alpaca_websocket_invalid_message: ") + exception.what();
      close_socket();
      return;
    }
    read_next();
  }

  void schedule_tick() {
    timer_.expires_after(std::chrono::milliseconds(100));
    timer_.async_wait(beast::bind_front_handler(&AlpacaWebSocketSession::on_tick, shared_from_this()));
  }

  void on_tick(beast::error_code error) {
    if (error == asio::error::operation_aborted) return;
    if (error) return fail("timer", error);
    if (!running_) {
      close_socket();
      return;
    }
    if (std::chrono::steady_clock::now() - last_receive_ >= std::chrono::seconds(120)) {
      error_ = "alpaca_websocket_stale";
      close_socket();
      return;
    }
    if (authenticated_) {
      const auto desired = subscriptions_.desired();
      const auto additions = difference(desired, sent_symbols_);
      const auto removals = difference(sent_symbols_, desired);
      if (!additions.empty()) queue_write(simtrade::market::subscription_message("subscribe", additions).dump());
      if (!removals.empty()) queue_write(simtrade::market::subscription_message("unsubscribe", removals).dump());
      sent_symbols_ = desired;
    }
    schedule_tick();
  }

  void queue_write(std::string message) {
    const bool write_in_progress = !write_queue_.empty();
    write_queue_.push_back(std::move(message));
    if (!write_in_progress) write_next();
  }

  void write_next() {
    websocket_.text(true);
    websocket_.async_write(asio::buffer(write_queue_.front()),
                           beast::bind_front_handler(&AlpacaWebSocketSession::on_write, shared_from_this()));
  }

  void on_write(beast::error_code error, std::size_t) {
    if (error) {
      if (closing_ && error == asio::error::operation_aborted) return;
      return fail("write", error);
    }
    write_queue_.pop_front();
    if (!write_queue_.empty()) write_next();
  }

  void close_socket() {
    if (closing_) return;
    closing_ = true;
    timer_.cancel();
    connection_handler_(false);
    if (!websocket_.is_open()) {
      io_.stop();
      return;
    }
    websocket_.async_close(websocket::close_code::normal,
                           beast::bind_front_handler(&AlpacaWebSocketSession::on_close, shared_from_this()));
  }

  void on_close(beast::error_code error) {
    if (error && error != websocket::error::closed && error != asio::error::operation_aborted && error_.empty()) {
      error_ = "alpaca_websocket_close_failed: " + error.message();
    }
    io_.stop();
  }

  void fail(const std::string& stage, beast::error_code error) {
    if (closing_) return;
    error_ = "alpaca_websocket_" + stage + "_failed: " + error.message();
    closing_ = true;
    timer_.cancel();
    connection_handler_(false);
    beast::error_code ignored;
    beast::get_lowest_layer(websocket_).socket().shutdown(tcp::socket::shutdown_both, ignored);
    beast::get_lowest_layer(websocket_).socket().close(ignored);
    io_.stop();
  }

  asio::io_context& io_;
  tcp::resolver resolver_;
  websocket::stream<beast::ssl_stream<beast::tcp_stream>> websocket_;
  asio::steady_timer timer_;
  beast::flat_buffer buffer_;
  WebSocketUrl url_;
  std::string authentication_;
  simtrade::market::SubscriptionRegistry& subscriptions_;
  const std::atomic<bool>& running_;
  MessageHandler message_handler_;
  ConnectionHandler connection_handler_;
  std::deque<std::string> write_queue_;
  std::set<std::string> sent_symbols_;
  std::chrono::steady_clock::time_point last_receive_{};
  std::string error_;
  bool authenticated_{false};
  bool closing_{false};
};

}  // namespace

namespace simtrade::market {

AlpacaMarketDataStream::AlpacaMarketDataStream(const config::Config& config,
                                               const cache::RedisClient& redis)
    : config_(config), redis_(redis), subscriptions_(config.alpaca_max_stream_symbols) {}

AlpacaMarketDataStream::~AlpacaMarketDataStream() { stop(); }

void AlpacaMarketDataStream::start(EventHandler handler) {
  if (running_.exchange(true)) return;
  {
    std::scoped_lock lock(mutex_);
    handler_ = std::move(handler);
  }
  worker_ = std::thread([this] { run(); });
}

void AlpacaMarketDataStream::stop() {
  if (!running_.exchange(false)) return;
  condition_.notify_all();
  if (worker_.joinable()) worker_.join();
}

SubscriptionResult AlpacaMarketDataStream::subscribe(const std::string& symbol) {
  const auto result = subscriptions_.request(symbol);
  condition_.notify_all();
  return result;
}

bool AlpacaMarketDataStream::unsubscribe(const std::string& symbol) {
  const bool removed = subscriptions_.release(symbol);
  condition_.notify_all();
  return removed;
}

MarketDataHealth AlpacaMarketDataStream::health() const {
  std::scoped_lock lock(mutex_);
  return {
      .connected = connected_,
      .feed = config_.alpaca_data_feed,
      .subscribed_symbol_count = subscriptions_.confirmed().size(),
      .maximum_symbol_count = subscriptions_.maximum(),
      .last_message_at = last_message_at_,
      .last_error = last_error_,
      .reconnect_count = reconnect_count_,
  };
}

const SubscriptionRegistry& AlpacaMarketDataStream::subscriptions() const noexcept { return subscriptions_; }

void AlpacaMarketDataStream::set_subscription_handlers(ConfirmationHandler confirmation_handler,
                                                       ConnectionHandler connection_handler) {
  std::scoped_lock lock(mutex_);
  confirmation_handler_ = std::move(confirmation_handler);
  connection_handler_ = std::move(connection_handler);
}

void AlpacaMarketDataStream::run() {
  std::uint32_t backoff_seconds = 1;
  std::mt19937 random(std::random_device{}());
  std::uniform_int_distribution<int> jitter(0, 500);
  while (running_) {
    if (config_.alpaca_api_key_id.empty() || config_.alpaca_api_secret_key.empty()) {
      {
        std::scoped_lock lock(mutex_);
        last_error_ = "alpaca_credentials_missing";
      }
      std::unique_lock lock(mutex_);
      condition_.wait_for(lock, std::chrono::seconds(5), [this] { return !running_; });
      continue;
    }
    try {
      connect_and_stream();
      backoff_seconds = 1;
    } catch (const std::exception& exception) {
      {
        std::scoped_lock lock(mutex_);
        connected_ = false;
        last_error_ = exception.what();
        ++reconnect_count_;
      }
      subscriptions_.disconnected();
      publish_status(false);
      std::cerr << nlohmann::json({{"level", "warning"},
                                   {"event", "alpaca_stream_disconnected"},
                                   {"message", exception.what()}}).dump() << std::endl;
      std::unique_lock lock(mutex_);
      condition_.wait_for(lock,
                          std::chrono::milliseconds(backoff_seconds * 1000 + jitter(random)),
                          [this] { return !running_; });
      backoff_seconds = std::min<std::uint32_t>(30, backoff_seconds * 2);
    }
  }
}

void AlpacaMarketDataStream::connect_and_stream() {
  asio::io_context io;
  ssl::context ssl_context(ssl::context::tls_client);
  ssl_context.set_default_verify_paths();
  ssl_context.set_verify_mode(ssl::verify_peer);

  auto session = std::make_shared<AlpacaWebSocketSession>(
      io,
      ssl_context,
      parse_websocket_url(config_.alpaca_data_ws_url),
      authentication_message(config_.alpaca_api_key_id, config_.alpaca_api_secret_key).dump(),
      subscriptions_,
      running_,
      [this](const std::string& message) { handle_message(message); },
      [this](bool connected) {
        ConnectionHandler connection_handler;
        {
          std::scoped_lock lock(mutex_);
          connected_ = connected;
          connection_handler = connection_handler_;
          if (connected) {
            last_message_at_ = current_iso8601();
            last_error_.clear();
          }
        }
        if (connection_handler) connection_handler(connected);
        publish_status(connected);
      });
  session->start();
  io.run();

  subscriptions_.disconnected();
  {
    std::scoped_lock lock(mutex_);
    connected_ = false;
  }
  if (!session->error().empty()) throw std::runtime_error(session->error());
}

void AlpacaMarketDataStream::handle_message(const std::string& message) {
  const auto document = nlohmann::json::parse(message);
  const auto values = document.is_array() ? document : nlohmann::json::array({document});
  bool acknowledgement = false;
  for (const auto& value : values) {
    if (value.is_object() && value.value("T", "") == "subscription") acknowledgement = true;
  }
  if (acknowledgement) {
    ConfirmationHandler confirmation_handler;
    {
      std::scoped_lock lock(mutex_);
      confirmation_handler = confirmation_handler_;
    }
    if (confirmation_handler) confirmation_handler(subscriptions_.confirmed());
  }
  const auto events = normalize_alpaca_events(document, config_.alpaca_data_feed);
  for (const auto& event : events) {
    cache_normalized_event(event, [this](const std::string& key, const std::string& value) {
      static_cast<void>(redis_.set(key, value));
    });
    EventHandler handler;
    {
      std::scoped_lock lock(mutex_);
      last_message_at_ = event.payload.value("cachedAt", current_iso8601());
      handler = handler_;
    }
    if (handler) handler(event.payload);
  }
}

void AlpacaMarketDataStream::publish_status(bool connected) {
  EventHandler handler;
  {
    std::scoped_lock lock(mutex_);
    handler = handler_;
  }
  const auto confirmed = subscriptions_.confirmed();
  for (const auto& symbol : subscriptions_.desired()) {
    const bool live = connected && confirmed.contains(symbol);
    nlohmann::json payload{{"type", "status"},
                           {"symbol", symbol},
                           {"connected", connected},
                           {"source", "alpaca_" + config_.alpaca_data_feed},
                           {"live", live},
                           {"timestamp", current_iso8601()},
                           {"cachedAt", current_iso8601()}};
    static_cast<void>(redis_.set("market:status:" + symbol, payload.dump()));
    if (handler) handler(payload);
  }
}

}  // namespace simtrade::market
