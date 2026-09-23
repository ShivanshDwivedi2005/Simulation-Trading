#include "api/HttpServer.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace http = boost::beast::http;

std::size_t append_response(char* data, std::size_t size, std::size_t count, void* target) {
  const auto bytes = size * count;
  static_cast<std::string*>(target)->append(data, bytes);
  return bytes;
}

nlohmann::json authenticated_get(const std::string& url,
                                 const std::string& api_key,
                                 const std::string& api_secret) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("market_data_client_unavailable");

  std::string response_body;
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, ("APCA-API-KEY-ID: " + api_key).c_str());
  headers = curl_slist_append(headers, ("APCA-API-SECRET-KEY: " + api_secret).c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "simtrade-api/0.1");

  const auto result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK) throw std::runtime_error("market_data_request_failed");
  if (status == 401 || status == 403) throw std::runtime_error("alpaca_credentials_or_feed_rejected");
  if (status == 429) throw std::runtime_error("alpaca_rate_limit_reached");
  if (status < 200 || status >= 300) throw std::runtime_error("alpaca_http_" + std::to_string(status));
  return nlohmann::json::parse(response_body);
}

bool valid_symbol(const std::string& symbol) {
  return !symbol.empty() && symbol.size() <= 12 && std::all_of(symbol.begin(), symbol.end(), [](unsigned char character) {
    return std::isalnum(character) || character == '.' || character == '-';
  });
}

nlohmann::json alpaca_quote(const simtrade::config::Config& config, std::string symbol) {
  if (config.alpaca_api_key_id.empty() || config.alpaca_api_secret_key.empty()) {
    throw std::runtime_error("alpaca_not_configured");
  }
  std::transform(symbol.begin(), symbol.end(), symbol.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
  if (!valid_symbol(symbol)) throw std::runtime_error("invalid_symbol");

  auto base_url = config.alpaca_data_rest_url;
  while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
  const auto suffix = "?symbols=" + symbol + "&feed=" + config.alpaca_data_feed;
  const auto quotes = authenticated_get(base_url + "/v2/stocks/quotes/latest" + suffix,
                                        config.alpaca_api_key_id,
                                        config.alpaca_api_secret_key);
  const auto trades = authenticated_get(base_url + "/v2/stocks/trades/latest" + suffix,
                                        config.alpaca_api_key_id,
                                        config.alpaca_api_secret_key);
  if (!quotes.contains("quotes") || !quotes["quotes"].contains(symbol) ||
      !trades.contains("trades") || !trades["trades"].contains(symbol)) {
    throw std::runtime_error("quote_unavailable");
  }

  const auto& quote = quotes["quotes"][symbol];
  const auto& trade = trades["trades"][symbol];
  const double last = trade.value("p", 0.0);
  double bid = quote.value("bp", 0.0);
  double ask = quote.value("ap", 0.0);
  if (bid <= 0.0) bid = last;
  if (ask <= 0.0) ask = last;

  return {
      {"symbol", symbol},
      {"bid", bid},
      {"ask", ask},
      {"last", last},
      {"timestamp", quote.value("t", trade.value("t", ""))},
      {"source", "alpaca"},
      {"feed", config.alpaca_data_feed},
  };
}

struct UploadBuffer {
  std::string content;
  std::size_t offset{0};
};

std::size_t read_upload(char* target, std::size_t size, std::size_t count, void* source) {
  auto& buffer = *static_cast<UploadBuffer*>(source);
  const auto capacity = size * count;
  const auto remaining = buffer.content.size() - buffer.offset;
  const auto bytes = std::min(capacity, remaining);
  if (bytes > 0) {
    std::copy_n(buffer.content.data() + buffer.offset, bytes, target);
    buffer.offset += bytes;
  }
  return bytes;
}

std::string mailbox_from(const std::string& sender) {
  const auto open = sender.find('<');
  const auto close = sender.find('>', open == std::string::npos ? 0 : open + 1);
  if (open != std::string::npos && close != std::string::npos) return sender.substr(open + 1, close - open - 1);
  return sender;
}

bool smtp_configured(const simtrade::config::Config& config) {
  return !config.smtp_host.empty() && config.smtp_host != "smtp.example.com" &&
         !config.smtp_username.empty() && !config.smtp_password.empty() &&
         config.smtp_from.find("@localhost") == std::string::npos;
}

bool send_otp_email(const simtrade::config::Config& config,
                    const std::string& recipient,
                    const std::string& otp,
                    const std::string& purpose) {
  if (!smtp_configured(config)) return false;
  CURL* curl = curl_easy_init();
  if (curl == nullptr) return false;

  const bool password_reset = purpose == "RESET_PASSWORD";
  const auto subject = password_reset ? "Reset your SimTrade password" : "Your SimTrade verification code";
  const auto description = password_reset ? "password reset code" : "verification code";

  UploadBuffer upload{
      "To: <" + recipient + ">\r\n"
      "From: " + config.smtp_from + "\r\n"
      "Subject: " + subject + "\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n\r\n"
      "Your SimTrade " + description + " is " + otp + ".\r\n"
      "It expires in " + std::to_string(config.otp_ttl_seconds / 60) + " minutes.\r\n"
      "If you did not request this code, you can ignore this email.\r\n"};
  const auto smtp_url = "smtp://" + config.smtp_host + ":" + std::to_string(config.smtp_port);
  const auto sender = "<" + mailbox_from(config.smtp_from) + ">";
  const auto recipient_address = "<" + recipient + ">";
  curl_slist* recipients = nullptr;
  recipients = curl_slist_append(recipients, recipient_address.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, smtp_url.c_str());
  if (!config.smtp_username.empty()) curl_easy_setopt(curl, CURLOPT_USERNAME, config.smtp_username.c_str());
  if (!config.smtp_password.empty()) curl_easy_setopt(curl, CURLOPT_PASSWORD, config.smtp_password.c_str());
  if (config.smtp_use_tls) curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
  curl_easy_setopt(curl, CURLOPT_MAIL_FROM, sender.c_str());
  curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_upload);
  curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

  const auto result = curl_easy_perform(curl);
  curl_slist_free_all(recipients);
  curl_easy_cleanup(curl);
  return result == CURLE_OK;
}

std::string generate_otp() {
  std::random_device source;
  std::uniform_int_distribution<int> distribution(0, 999999);
  std::ostringstream code;
  code << std::setw(6) << std::setfill('0') << distribution(source);
  return code.str();
}

bool valid_email(const std::string& email) {
  const auto at = email.find('@');
  const auto dot = email.rfind('.');
  return at != std::string::npos && at > 0 && dot != std::string::npos && dot > at + 1 && dot + 1 < email.size();
}

std::string bearer_token(const http::request<http::string_body>& request) {
  const auto iterator = request.find(http::field::authorization);
  if (iterator == request.end()) return "";
  const std::string value(iterator->value());
  constexpr std::string_view prefix = "Bearer ";
  if (!value.starts_with(prefix)) return "";
  return value.substr(prefix.size());
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string allowed_cors_origin(const http::request<http::string_body>& request,
                                const std::string& configured_origins) {
  const auto header = request.find(http::field::origin);
  if (header == request.end()) return "";
  const std::string request_origin(header->value());

  std::size_t start = 0;
  while (start <= configured_origins.size()) {
    const auto end = configured_origins.find(',', start);
    const auto allowed = trim(configured_origins.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (allowed == "*" || allowed == request_origin) return allowed == "*" ? "*" : request_origin;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return "";
}

}  // namespace

namespace simtrade::api {
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket socket,
              const config::Config& config,
              database::PostgresRepository& postgres,
              const cache::RedisClient& redis)
      : stream_(std::move(socket)), config_(config), postgres_(postgres), redis_(redis) {}

  void run() { read(); }

 private:
  void read() {
    request_ = {};
    http::async_read(stream_, buffer_, request_, beast::bind_front_handler(&HttpSession::on_read, shared_from_this()));
  }

  void on_read(beast::error_code error, std::size_t) {
    if (error == http::error::end_of_stream) return close();
    if (error) return;
    write(route());
  }

  http::response<http::string_body> json_response(http::status status, const nlohmann::json& body) const {
    http::response<http::string_body> response{status, request_.version()};
    response.set(http::field::server, "simtrade-beast");
    response.set(http::field::content_type, "application/json; charset=utf-8");
    const auto cors_origin = allowed_cors_origin(request_, config_.cors_allowed_origins);
    if (!cors_origin.empty()) response.set(http::field::access_control_allow_origin, cors_origin);
    response.set("Vary", "Origin");
    response.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
    response.set(http::field::access_control_allow_headers, "Authorization, Content-Type");
    response.set(http::field::access_control_max_age, "600");
    response.keep_alive(request_.keep_alive());
    if (body.is_null()) {
      response.body().clear();
    } else {
      response.body() = body.dump();
    }
    response.prepare_payload();
    return response;
  }

  http::response<http::string_body> route() {
    const auto target = std::string(request_.target());
    if (request_.method() == http::verb::options) {
      if (request_.find(http::field::origin) != request_.end() &&
          allowed_cors_origin(request_, config_.cors_allowed_origins).empty()) {
        return json_response(http::status::forbidden, {{"error", "cors_origin_not_allowed"}});
      }
      return json_response(http::status::no_content, nullptr);
    }

    try {
      if (request_.method() == http::verb::get && (target == "/health" || target == "/ready")) {
        const bool ready = postgres_.healthy() && redis_.healthy();
        return json_response(ready ? http::status::ok : http::status::service_unavailable,
                             {{"status", ready ? "ready" : "degraded"},
                              {"services", {{"postgres", postgres_.status()}, {"redis", redis_.status()},
                                            {"alpaca", config_.alpaca_api_key_id.empty() ? "not_configured" : "configured"},
                                            {"smtp", smtp_configured(config_) ? "configured" : "not_configured"}}}});
      }

      if (request_.method() == http::verb::post && target == "/api/v1/auth/register") {
        const auto input = nlohmann::json::parse(request_.body());
        const auto name = input.value("name", "");
        const auto email = input.value("email", "");
        const auto password = input.value("password", "");
        if (name.size() < 2 || !valid_email(email) || password.size() < 8) {
          return json_response(http::status::bad_request, {{"error", "invalid_registration"}, {"message", "Name, a valid email, and a password of at least 8 characters are required."}});
        }
        const auto otp = generate_otp();
        auto result = postgres_.register_user(name, email, password, otp, config_.otp_pepper, config_.otp_ttl_seconds);
        if (!send_otp_email(config_, email, otp, "VERIFY_EMAIL")) {
          return json_response(http::status::service_unavailable,
                               {{"error", "otp_delivery_unavailable"},
                                {"message", "We could not send the verification email. Check the mail settings and try again."}});
        }
        result["delivery"] = "email";
        result["message"] = "Verification code sent.";
        return json_response(http::status::created, result);
      }

      if (request_.method() == http::verb::post && target == "/api/v1/auth/verify-otp") {
        const auto input = nlohmann::json::parse(request_.body());
        const auto email = input.value("email", "");
        const auto otp = input.value("otp", "");
        if (!valid_email(email) || otp.size() != 6) return json_response(http::status::bad_request, {{"error", "invalid_otp_format"}});
        auto verification = postgres_.verify_otp(email, otp, config_.otp_pepper);
        if (!verification) return json_response(http::status::unauthorized, {{"error", "invalid_or_expired_otp"}, {"message", "The verification code is incorrect, expired, or has too many failed attempts."}});
        (*verification)["message"] = "Email verified. Sign in to continue.";
        return json_response(http::status::ok, *verification);
      }

      if (request_.method() == http::verb::post && target == "/api/v1/auth/login") {
        const auto input = nlohmann::json::parse(request_.body());
        const auto session = postgres_.login(input.value("email", ""), input.value("password", ""), config_.access_token_ttl_seconds);
        if (!session) return json_response(http::status::unauthorized, {{"error", "invalid_credentials"}, {"message", "Email or password is incorrect, or the email has not been verified."}});
        return json_response(http::status::ok, *session);
      }

      if (request_.method() == http::verb::post && target == "/api/v1/auth/password-reset/request") {
        const auto input = nlohmann::json::parse(request_.body());
        const auto email = input.value("email", "");
        if (!valid_email(email)) {
          return json_response(http::status::bad_request,
                               {{"error", "invalid_email"}, {"message", "Enter a valid email address."}});
        }
        if (!smtp_configured(config_)) {
          return json_response(http::status::service_unavailable,
                               {{"error", "otp_delivery_unavailable"},
                                {"message", "Password reset email is temporarily unavailable. Please try again later."}});
        }

        const auto otp = generate_otp();
        const auto challenge = postgres_.create_password_reset(
            email, otp, config_.otp_pepper, config_.otp_ttl_seconds);
        if (challenge && !send_otp_email(config_, (*challenge)["email"].get<std::string>(), otp, "RESET_PASSWORD")) {
          return json_response(http::status::service_unavailable,
                               {{"error", "otp_delivery_unavailable"},
                                {"message", "Password reset email is temporarily unavailable. Please try again later."}});
        }

        return json_response(http::status::accepted,
                             {{"message", "If an active account exists for that email, a password reset code has been sent."}});
      }

      if (request_.method() == http::verb::post && target == "/api/v1/auth/password-reset/confirm") {
        const auto input = nlohmann::json::parse(request_.body());
        const auto email = input.value("email", "");
        const auto otp = input.value("otp", "");
        const auto new_password = input.value("new_password", "");
        if (!valid_email(email) || otp.size() != 6 ||
            !std::all_of(otp.begin(), otp.end(), [](unsigned char character) { return std::isdigit(character); }) ||
            new_password.size() < 8) {
          return json_response(http::status::bad_request,
                               {{"error", "invalid_password_reset"},
                                {"message", "A valid email, 6-digit code, and password of at least 8 characters are required."}});
        }
        if (!postgres_.reset_password(email, otp, new_password, config_.otp_pepper)) {
          return json_response(http::status::unauthorized,
                               {{"error", "invalid_or_expired_otp"},
                                {"message", "The reset code is incorrect, expired, or has too many failed attempts."}});
        }
        return json_response(http::status::ok,
                             {{"message", "Password reset successfully. Sign in with your new password."}});
      }

      if (request_.method() == http::verb::get && target == "/api/v1/portfolio") {
        const auto token = bearer_token(request_);
        if (token.empty()) return json_response(http::status::unauthorized, {{"error", "missing_access_token"}});
        const auto portfolio = postgres_.portfolio(token);
        if (!portfolio) return json_response(http::status::unauthorized, {{"error", "invalid_or_expired_access_token"}});
        return json_response(http::status::ok, *portfolio);
      }

      if (request_.method() == http::verb::post && target == "/api/v1/orders") {
        const auto token = bearer_token(request_);
        if (token.empty()) return json_response(http::status::unauthorized, {{"error", "missing_access_token"}});
        const auto input = nlohmann::json::parse(request_.body());
        auto symbol = input.value("symbol", "");
        auto side = input.value("side", "");
        auto order_type = input.value("type", "");
        std::transform(symbol.begin(), symbol.end(), symbol.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
        std::transform(side.begin(), side.end(), side.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
        std::transform(order_type.begin(), order_type.end(), order_type.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
        const auto quote = alpaca_quote(config_, symbol);
        const auto order = postgres_.place_order(token,
                                                 symbol,
                                                 side,
                                                 order_type,
                                                 input.value("quantity", 0.0),
                                                 input.contains("limit_price") ? std::optional<double>(input["limit_price"].get<double>()) : std::nullopt,
                                                 input.contains("stop_price") ? std::optional<double>(input["stop_price"].get<double>()) : std::nullopt,
                                                 quote["bid"].get<double>(),
                                                 quote["ask"].get<double>());
        if (!order) return json_response(http::status::unauthorized, {{"error", "invalid_or_expired_access_token"}});
        return json_response(http::status::created, *order);
      }

      if (request_.method() == http::verb::get && target.rfind("/api/v1/market/", 0) == 0 && target.ends_with("/quote")) {
        const auto symbol = target.substr(15, target.size() - 15 - 6);
        return json_response(http::status::ok, alpaca_quote(config_, symbol));
      }

      return json_response(http::status::not_found, {{"error", "not_found"}, {"path", target}});
    } catch (const nlohmann::json::exception&) {
      return json_response(http::status::bad_request, {{"error", "invalid_json"}});
    } catch (const std::exception& exception) {
      const std::string message = exception.what();
      if (message == "email_already_registered") {
        return json_response(http::status::conflict, {{"error", message}, {"message", "An account with this email already exists."}});
      }
      if (message == "invalid_symbol") return json_response(http::status::bad_request, {{"error", message}});
      if (message == "invalid_quantity" || message == "invalid_side" || message == "invalid_order_type" ||
          message == "limit_price_required" || message == "stop_price_required" || message == "instrument_not_found") {
        return json_response(http::status::bad_request, {{"error", message}, {"message", message}});
      }
      if (message == "insufficient_buying_power" || message == "insufficient_position") {
        return json_response(http::status::unprocessable_entity, {{"error", message}, {"message", message}});
      }
      if (message == "alpaca_not_configured") return json_response(http::status::service_unavailable, {{"error", message}});
      return json_response(http::status::bad_gateway, {{"error", "upstream_or_database_error"}, {"message", message}});
    }
  }

  void write(http::response<http::string_body> response) {
    const bool close_after = response.need_eof();
    auto shared_response = std::make_shared<http::response<http::string_body>>(std::move(response));
    http::async_write(stream_, *shared_response, [self = shared_from_this(), shared_response, close_after](beast::error_code error, std::size_t) {
      if (error) return;
      if (close_after) return self->close();
      self->read();
    });
  }

  void close() {
    beast::error_code error;
    stream_.socket().shutdown(tcp::socket::shutdown_send, error);
  }

  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> request_;
  const config::Config& config_;
  database::PostgresRepository& postgres_;
  const cache::RedisClient& redis_;
};

HttpServer::HttpServer(net::io_context& io,
                       const config::Config& config,
                       database::PostgresRepository& postgres,
                       const cache::RedisClient& redis)
    : io_(io), config_(config), postgres_(postgres), redis_(redis), acceptor_(net::make_strand(io)) {
  const auto address = net::ip::make_address(config.http_host);
  const tcp::endpoint endpoint{address, config.http_port};
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(net::socket_base::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen(net::socket_base::max_listen_connections);
}

void HttpServer::run() { accept(); }

void HttpServer::accept() {
  acceptor_.async_accept(net::make_strand(io_), [this](beast::error_code error, tcp::socket socket) {
    if (!error) std::make_shared<HttpSession>(std::move(socket), config_, postgres_, redis_)->run();
    accept();
  });
}

}  // namespace simtrade::api
