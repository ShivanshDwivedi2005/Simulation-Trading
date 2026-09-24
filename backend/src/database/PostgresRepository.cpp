#include "database/PostgresRepository.hpp"

#include <pqxx/pqxx>

#include <array>
#include <cmath>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace {

std::string random_token() {
  std::array<unsigned char, 32> bytes{};
  std::random_device source;
  for (auto& byte : bytes) byte = static_cast<unsigned char>(source());
  std::ostringstream encoded;
  encoded << std::hex << std::setfill('0');
  for (const auto byte : bytes) encoded << std::setw(2) << static_cast<unsigned int>(byte);
  return encoded.str();
}

nlohmann::json issue_session(pqxx::work& transaction,
                             const std::string& user_id,
                             const std::string& name,
                             const std::string& email,
                             std::uint32_t ttl_seconds) {
  const auto token = random_token();
  transaction.exec_params(
      "INSERT INTO refresh_tokens (user_id, family_id, token_hash, expires_at) "
      "VALUES ($1::uuid, gen_random_uuid(), encode(digest($2, 'sha256'), 'hex'), now() + ($3 * interval '1 second'))",
      user_id,
      token,
      ttl_seconds);
  return {
      {"access_token", token},
      {"token_type", "Bearer"},
      {"expires_in", ttl_seconds},
      {"user", {{"id", user_id}, {"name", name}, {"email", email}}},
  };
}

}  // namespace

namespace simtrade::database {

PostgresRepository::PostgresRepository(const std::string& connection_string) {
  try {
    connection_ = std::make_unique<pqxx::connection>(connection_string);
    pqxx::work migration(*connection_);
    migration.exec("ALTER TABLE orders ADD COLUMN IF NOT EXISTS execution_price_source VARCHAR(64), "
                   "ADD COLUMN IF NOT EXISTS execution_price_timestamp TIMESTAMPTZ");
    migration.exec("ALTER TABLE trades ADD COLUMN IF NOT EXISTS price_source VARCHAR(64), "
                   "ADD COLUMN IF NOT EXISTS price_timestamp TIMESTAMPTZ");
    migration.commit();
  } catch (const std::exception& exception) {
    error_ = exception.what();
    connection_.reset();
  }
}

PostgresRepository::~PostgresRepository() = default;
bool PostgresRepository::healthy() const noexcept {
  return connection_ != nullptr && connection_->is_open();
}

std::string PostgresRepository::status() const {
  return healthy() ? "ok" : (error_.empty() ? "unavailable" : error_);
}

nlohmann::json PostgresRepository::register_user(const std::string& name,
                                                 const std::string& email,
                                                 const std::string& password,
                                                 const std::string& otp,
                                                 const std::string& otp_pepper,
                                                 std::uint32_t otp_ttl_seconds) {
  std::scoped_lock lock(mutex_);
  if (!healthy()) throw std::runtime_error("database_unavailable");

  pqxx::work transaction(*connection_);
  const auto existing = transaction.exec_params(
      "SELECT user_id::text, email_verified_at IS NOT NULL AS verified FROM users WHERE lower(email) = lower($1) FOR UPDATE",
      email);

  std::string user_id;
  if (!existing.empty()) {
    if (existing[0]["verified"].as<bool>()) throw std::runtime_error("email_already_registered");
    user_id = existing[0]["user_id"].as<std::string>();
    transaction.exec_params(
        "UPDATE users SET name = $2, status = 'PENDING_VERIFICATION', updated_at = now() WHERE user_id = $1::uuid",
        user_id,
        name);
  } else {
    const auto inserted = transaction.exec_params(
        "INSERT INTO users (name, email, status) VALUES ($1, lower($2), 'PENDING_VERIFICATION') RETURNING user_id::text",
        name,
        email);
    user_id = inserted[0][0].as<std::string>();
  }

  transaction.exec_params(
      "INSERT INTO user_credentials (user_id, password_hash, password_algorithm) "
      "VALUES ($1::uuid, crypt($2, gen_salt('bf', 12)), 'bcrypt') "
      "ON CONFLICT (user_id) DO UPDATE SET password_hash = excluded.password_hash, password_algorithm = excluded.password_algorithm, password_changed_at = now()",
      user_id,
      password);
  transaction.exec_params(
      "UPDATE otp_challenges SET consumed_at = now() WHERE user_id = $1::uuid AND purpose = 'VERIFY_EMAIL' AND consumed_at IS NULL",
      user_id);
  const auto challenge = transaction.exec_params(
      "INSERT INTO otp_challenges (user_id, purpose, otp_hash, expires_at) "
      "VALUES ($1::uuid, 'VERIFY_EMAIL', encode(digest($2, 'sha256'), 'hex'), now() + ($3 * interval '1 second')) "
      "RETURNING challenge_id::text, expires_at",
      user_id,
      otp + otp_pepper,
      otp_ttl_seconds);
  transaction.commit();

  return {
      {"challenge_id", challenge[0]["challenge_id"].as<std::string>()},
      {"email", email},
      {"expires_at", challenge[0]["expires_at"].as<std::string>()},
  };
}

std::optional<nlohmann::json> PostgresRepository::verify_otp(const std::string& email,
                                                            const std::string& otp,
                                                            const std::string& otp_pepper) {
  std::scoped_lock lock(mutex_);
  if (!healthy()) throw std::runtime_error("database_unavailable");

  pqxx::work transaction(*connection_);
  const auto users = transaction.exec_params(
      "SELECT user_id::text, name, email FROM users WHERE lower(email) = lower($1) FOR UPDATE",
      email);
  if (users.empty()) return std::nullopt;

  const auto user_id = users[0]["user_id"].as<std::string>();
  const auto challenges = transaction.exec_params(
      "SELECT challenge_id::text, expires_at > now() AS unexpired, attempt_count, "
      "otp_hash = encode(digest($2, 'sha256'), 'hex') AS matches "
      "FROM otp_challenges WHERE user_id = $1::uuid AND purpose = 'VERIFY_EMAIL' AND consumed_at IS NULL "
      "ORDER BY created_at DESC LIMIT 1 FOR UPDATE",
      user_id,
      otp + otp_pepper);
  if (challenges.empty()) return std::nullopt;

  const auto challenge_id = challenges[0]["challenge_id"].as<std::string>();
  const auto attempt_count = challenges[0]["attempt_count"].as<int>();
  const bool valid = challenges[0]["unexpired"].as<bool>() && challenges[0]["matches"].as<bool>() && attempt_count < 5;
  if (!valid) {
    transaction.exec_params("UPDATE otp_challenges SET attempt_count = attempt_count + 1 WHERE challenge_id = $1::uuid", challenge_id);
    transaction.commit();
    return std::nullopt;
  }

  transaction.exec_params("UPDATE otp_challenges SET consumed_at = now() WHERE challenge_id = $1::uuid", challenge_id);
  transaction.exec_params(
      "UPDATE users SET email_verified_at = COALESCE(email_verified_at, now()), status = 'ACTIVE', updated_at = now() WHERE user_id = $1::uuid",
      user_id);
  transaction.exec_params(
      "INSERT INTO trading_accounts (user_id) SELECT $1::uuid WHERE NOT EXISTS (SELECT 1 FROM trading_accounts WHERE user_id = $1::uuid)",
      user_id);
  transaction.commit();
  return nlohmann::json{
      {"verified", true},
      {"user", {{"id", user_id},
                {"name", users[0]["name"].as<std::string>()},
                {"email", users[0]["email"].as<std::string>()}}},
  };
}

std::optional<nlohmann::json> PostgresRepository::create_password_reset(const std::string& email,
                                                                        const std::string& otp,
                                                                        const std::string& otp_pepper,
                                                                        std::uint32_t otp_ttl_seconds) {
  std::scoped_lock lock(mutex_);
  if (!healthy()) throw std::runtime_error("database_unavailable");

  pqxx::work transaction(*connection_);
  const auto users = transaction.exec_params(
      "SELECT user_id::text, email FROM users "
      "WHERE lower(email) = lower($1) AND status = 'ACTIVE' AND email_verified_at IS NOT NULL FOR UPDATE",
      email);
  if (users.empty()) return std::nullopt;

  const auto user_id = users[0]["user_id"].as<std::string>();
  transaction.exec_params(
      "UPDATE otp_challenges SET consumed_at = now() "
      "WHERE user_id = $1::uuid AND purpose = 'RESET_PASSWORD' AND consumed_at IS NULL",
      user_id);
  const auto challenge = transaction.exec_params(
      "INSERT INTO otp_challenges (user_id, purpose, otp_hash, expires_at) "
      "VALUES ($1::uuid, 'RESET_PASSWORD', encode(digest($2, 'sha256'), 'hex'), now() + ($3 * interval '1 second')) "
      "RETURNING challenge_id::text, expires_at",
      user_id,
      otp + otp_pepper,
      otp_ttl_seconds);
  transaction.commit();

  return nlohmann::json{
      {"challenge_id", challenge[0]["challenge_id"].as<std::string>()},
      {"email", users[0]["email"].as<std::string>()},
      {"expires_at", challenge[0]["expires_at"].as<std::string>()},
  };
}

bool PostgresRepository::reset_password(const std::string& email,
                                        const std::string& otp,
                                        const std::string& new_password,
                                        const std::string& otp_pepper) {
  std::scoped_lock lock(mutex_);
  if (!healthy()) throw std::runtime_error("database_unavailable");

  pqxx::work transaction(*connection_);
  const auto users = transaction.exec_params(
      "SELECT user_id::text FROM users "
      "WHERE lower(email) = lower($1) AND status = 'ACTIVE' AND email_verified_at IS NOT NULL FOR UPDATE",
      email);
  if (users.empty()) return false;

  const auto user_id = users[0]["user_id"].as<std::string>();
  const auto challenges = transaction.exec_params(
      "SELECT challenge_id::text, expires_at > now() AS unexpired, attempt_count, "
      "otp_hash = encode(digest($2, 'sha256'), 'hex') AS matches "
      "FROM otp_challenges WHERE user_id = $1::uuid AND purpose = 'RESET_PASSWORD' AND consumed_at IS NULL "
      "ORDER BY created_at DESC LIMIT 1 FOR UPDATE",
      user_id,
      otp + otp_pepper);
  if (challenges.empty()) return false;

  const auto challenge_id = challenges[0]["challenge_id"].as<std::string>();
  const auto attempt_count = challenges[0]["attempt_count"].as<int>();
  const bool valid = challenges[0]["unexpired"].as<bool>() && challenges[0]["matches"].as<bool>() && attempt_count < 5;
  if (!valid) {
    transaction.exec_params(
        "UPDATE otp_challenges SET attempt_count = attempt_count + 1 WHERE challenge_id = $1::uuid",
        challenge_id);
    transaction.commit();
    return false;
  }

  transaction.exec_params(
      "UPDATE user_credentials SET password_hash = crypt($2, gen_salt('bf', 12)), "
      "password_algorithm = 'bcrypt', password_changed_at = now() WHERE user_id = $1::uuid",
      user_id,
      new_password);
  transaction.exec_params("UPDATE otp_challenges SET consumed_at = now() WHERE challenge_id = $1::uuid", challenge_id);
  transaction.exec_params(
      "UPDATE refresh_tokens SET revoked_at = now() WHERE user_id = $1::uuid AND revoked_at IS NULL",
      user_id);
  transaction.commit();
  return true;
}

std::optional<nlohmann::json> PostgresRepository::login(const std::string& email,
                                                       const std::string& password,
                                                       std::uint32_t access_token_ttl_seconds) {
  std::scoped_lock lock(mutex_);
  if (!healthy()) throw std::runtime_error("database_unavailable");

  pqxx::work transaction(*connection_);
  const auto users = transaction.exec_params(
      "SELECT u.user_id::text, u.name, u.email FROM users u "
      "JOIN user_credentials c ON c.user_id = u.user_id "
      "WHERE lower(u.email) = lower($1) AND u.status = 'ACTIVE' AND u.email_verified_at IS NOT NULL "
      "AND c.password_hash = crypt($2, c.password_hash) FOR UPDATE OF u",
      email,
      password);
  if (users.empty()) return std::nullopt;

  auto session = issue_session(transaction,
                               users[0]["user_id"].as<std::string>(),
                               users[0]["name"].as<std::string>(),
                               users[0]["email"].as<std::string>(),
                               access_token_ttl_seconds);
  transaction.commit();
  return session;
}

std::optional<nlohmann::json> PostgresRepository::portfolio(const std::string& access_token) const {
  std::scoped_lock lock(mutex_);
  if (!healthy()) throw std::runtime_error("database_unavailable");

  pqxx::read_transaction transaction(*connection_);
  const auto accounts = transaction.exec_params(
      "SELECT a.account_id::text, a.user_id::text, a.base_currency, a.cash::double precision, a.reserved_cash::double precision "
      "FROM refresh_tokens t JOIN trading_accounts a ON a.user_id = t.user_id "
      "WHERE t.token_hash = encode(digest($1, 'sha256'), 'hex') AND t.revoked_at IS NULL AND t.expires_at > now() "
      "AND a.status = 'ACTIVE' ORDER BY a.created_at LIMIT 1",
      access_token);
  if (accounts.empty()) return std::nullopt;

  const auto account_id = accounts[0]["account_id"].as<std::string>();
  const double cash = accounts[0]["cash"].as<double>();
  const double reserved_cash = accounts[0]["reserved_cash"].as<double>();
  const auto rows = transaction.exec_params(
      "SELECT i.symbol, p.quantity::double precision, p.average_price::double precision, p.realized_pnl::double precision "
      "FROM positions p JOIN instruments i ON i.instrument_id = p.instrument_id "
      "WHERE p.account_id = $1::uuid AND p.quantity <> 0 ORDER BY i.symbol",
      account_id);
  const auto order_rows = transaction.exec_params(
      "SELECT o.order_id::text, i.symbol, o.side, o.order_type, o.quantity::double precision, "
      "COALESCE(o.average_fill_price, o.limit_price, o.stop_price, 0)::double precision AS price, o.status, o.created_at "
      "FROM orders o JOIN instruments i ON i.instrument_id = o.instrument_id "
      "WHERE o.account_id = $1::uuid ORDER BY o.created_at DESC LIMIT 100",
      account_id);

  nlohmann::json positions = nlohmann::json::array();
  double positions_at_cost = 0.0;
  double realized_pnl = 0.0;
  for (const auto& row : rows) {
    const double quantity = row["quantity"].as<double>();
    const double average_price = row["average_price"].as<double>();
    positions_at_cost += quantity * average_price;
    realized_pnl += row["realized_pnl"].as<double>();
    positions.push_back({
        {"symbol", row["symbol"].as<std::string>()},
        {"quantity", quantity},
        {"average_price", average_price},
        {"realized_pnl", row["realized_pnl"].as<double>()},
    });
  }

  nlohmann::json orders = nlohmann::json::array();
  for (const auto& row : order_rows) {
    orders.push_back({
        {"id", row["order_id"].as<std::string>()},
        {"symbol", row["symbol"].as<std::string>()},
        {"side", row["side"].as<std::string>()},
        {"type", row["order_type"].as<std::string>()},
        {"quantity", row["quantity"].as<double>()},
        {"price", row["price"].as<double>()},
        {"status", row["status"].as<std::string>()},
        {"created_at", row["created_at"].as<std::string>()},
    });
  }

  return nlohmann::json{
      {"account_id", account_id},
      {"currency", accounts[0]["base_currency"].as<std::string>()},
      {"cash", cash},
      {"buying_power", (cash - reserved_cash) * 2.0},
      {"portfolio_value", cash + positions_at_cost},
      {"realized_pnl", realized_pnl},
      {"unrealized_pnl", 0.0},
      {"positions", positions},
      {"orders", orders},
  };
}

std::optional<nlohmann::json> PostgresRepository::place_order(const std::string& access_token,
                                                              const std::string& symbol,
                                                              const std::string& side,
                                                              const std::string& order_type,
                                                              double quantity,
                                                              std::optional<double> limit_price,
                                                              std::optional<double> stop_price,
                                                              double bid,
                                                              double ask,
                                                              const std::string& price_source,
                                                              const std::string& price_timestamp) {
  std::scoped_lock lock(mutex_);
  if (!healthy()) throw std::runtime_error("database_unavailable");
  if (!std::isfinite(quantity) || quantity <= 0.0) throw std::runtime_error("invalid_quantity");
  if (side != "BUY" && side != "SELL") throw std::runtime_error("invalid_side");
  if (order_type != "MARKET" && order_type != "LIMIT" && order_type != "STOP" && order_type != "STOP_LIMIT") {
    throw std::runtime_error("invalid_order_type");
  }
  if ((order_type == "LIMIT" || order_type == "STOP_LIMIT") && (!limit_price || *limit_price <= 0.0)) {
    throw std::runtime_error("limit_price_required");
  }
  if ((order_type == "STOP" || order_type == "STOP_LIMIT") && (!stop_price || *stop_price <= 0.0)) {
    throw std::runtime_error("stop_price_required");
  }

  pqxx::work transaction(*connection_);
  const auto accounts = transaction.exec_params(
      "SELECT a.user_id::text, a.account_id::text, a.cash::double precision "
      "FROM refresh_tokens t JOIN trading_accounts a ON a.user_id = t.user_id "
      "WHERE t.token_hash = encode(digest($1, 'sha256'), 'hex') AND t.revoked_at IS NULL AND t.expires_at > now() "
      "AND a.status = 'ACTIVE' ORDER BY a.created_at LIMIT 1 FOR UPDATE OF a",
      access_token);
  if (accounts.empty()) return std::nullopt;

  const auto instruments = transaction.exec_params(
      "SELECT instrument_id::text FROM instruments WHERE symbol = $1 AND status = 'ACTIVE' LIMIT 1",
      symbol);
  if (instruments.empty()) throw std::runtime_error("instrument_not_found");

  const auto user_id = accounts[0]["user_id"].as<std::string>();
  const auto account_id = accounts[0]["account_id"].as<std::string>();
  const auto instrument_id = instruments[0]["instrument_id"].as<std::string>();
  const bool filled = order_type == "MARKET";
  const double fill_price = side == "BUY" ? ask : bid;
  if (filled && (!std::isfinite(fill_price) || fill_price <= 0.0)) throw std::runtime_error("invalid_market_quote");
  const double notional = filled ? fill_price * quantity : 0.0;

  if (filled && side == "BUY" && notional > accounts[0]["cash"].as<double>()) {
    throw std::runtime_error("insufficient_buying_power");
  }
  if (filled && side == "SELL") {
    const auto holdings = transaction.exec_params(
        "SELECT quantity::double precision FROM positions WHERE account_id = $1::uuid AND instrument_id = $2::uuid FOR UPDATE",
        account_id,
        instrument_id);
    if (holdings.empty() || holdings[0][0].as<double>() < quantity) throw std::runtime_error("insufficient_position");
  }

  const auto client_order_id = random_token().substr(0, 24);
  const auto order = transaction.exec_params(
      "INSERT INTO orders (user_id, account_id, instrument_id, client_order_id, side, order_type, quantity, remaining_quantity, limit_price, stop_price, status, accepted_at, filled_at, average_fill_price, execution_price_source, execution_price_timestamp) "
      "VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, $6, $7, $8, NULLIF($9, '')::numeric, NULLIF($10, '')::numeric, $11, now(), CASE WHEN $12 THEN now() ELSE NULL END, NULLIF($13, '')::numeric, CASE WHEN $12 THEN $14 ELSE NULL END, CASE WHEN $12 THEN $15::timestamptz ELSE NULL END) "
      "RETURNING order_id::text, created_at",
      user_id,
      account_id,
      instrument_id,
      client_order_id,
      side,
      order_type,
      quantity,
      filled ? 0.0 : quantity,
      limit_price ? std::to_string(*limit_price) : "",
      stop_price ? std::to_string(*stop_price) : "",
      filled ? "FILLED" : "ACCEPTED",
      filled,
      filled ? std::to_string(fill_price) : "",
      price_source,
      price_timestamp);

  if (filled) {
    const auto order_id = order[0]["order_id"].as<std::string>();
    transaction.exec_params(
        "INSERT INTO trades (order_id, user_id, account_id, instrument_id, side, quantity, price, price_source, price_timestamp) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5, $6, $7, $8, $9::timestamptz)",
        order_id,
        user_id,
        account_id,
        instrument_id,
        side,
        quantity,
        fill_price,
        price_source,
        price_timestamp);
    if (side == "BUY") {
      transaction.exec_params("UPDATE trading_accounts SET cash = cash - $2, version = version + 1, updated_at = now() WHERE account_id = $1::uuid", account_id, notional);
      transaction.exec_params(
          "INSERT INTO positions (account_id, user_id, instrument_id, quantity, average_price, opened_at) VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, now()) "
          "ON CONFLICT (account_id, instrument_id) DO UPDATE SET "
          "average_price = ((positions.quantity * positions.average_price) + ($4 * $5)) / (positions.quantity + $4), "
          "quantity = positions.quantity + $4, version = positions.version + 1, updated_at = now()",
          account_id,
          user_id,
          instrument_id,
          quantity,
          fill_price);
    } else {
      transaction.exec_params("UPDATE trading_accounts SET cash = cash + $2, version = version + 1, updated_at = now() WHERE account_id = $1::uuid", account_id, notional);
      transaction.exec_params(
          "UPDATE positions SET realized_pnl = realized_pnl + (($3 - average_price) * $2), quantity = quantity - $2, "
          "average_price = CASE WHEN quantity - $2 = 0 THEN 0 ELSE average_price END, version = version + 1, updated_at = now() "
          "WHERE account_id = $1::uuid AND instrument_id = $4::uuid",
          account_id,
          quantity,
          fill_price,
          instrument_id);
    }
  }

  transaction.commit();
  return nlohmann::json{
      {"id", order[0]["order_id"].as<std::string>()},
      {"symbol", symbol},
      {"side", side},
      {"type", order_type},
      {"quantity", quantity},
      {"price", filled ? fill_price : limit_price.value_or(stop_price.value_or(0.0))},
      {"status", filled ? "FILLED" : "ACCEPTED"},
      {"price_source", filled ? nlohmann::json(price_source) : nlohmann::json(nullptr)},
      {"price_timestamp", filled ? nlohmann::json(price_timestamp) : nlohmann::json(nullptr)},
      {"created_at", order[0]["created_at"].as<std::string>()},
  };
}

std::optional<nlohmann::json> PostgresRepository::cancel_order(const std::string& access_token,
                                                               const std::string& order_id) {
  std::scoped_lock lock(mutex_);
  if (!healthy()) throw std::runtime_error("database_unavailable");

  pqxx::work transaction(*connection_);
  const auto users = transaction.exec_params(
      "SELECT user_id::text FROM refresh_tokens "
      "WHERE token_hash = encode(digest($1, 'sha256'), 'hex') AND revoked_at IS NULL AND expires_at > now() LIMIT 1",
      access_token);
  if (users.empty()) return std::nullopt;

  const auto rows = transaction.exec_params(
      "UPDATE orders o SET status = 'CANCELLED', cancelled_at = now(), updated_at = now(), version = version + 1 "
      "FROM instruments i WHERE o.instrument_id = i.instrument_id AND o.order_id = $1::uuid "
      "AND o.user_id = $2::uuid AND o.status IN ('NEW', 'ACCEPTED', 'PARTIALLY_FILLED') "
      "RETURNING o.order_id::text, i.symbol, o.status",
      order_id,
      users[0][0].as<std::string>());
  if (rows.empty()) throw std::runtime_error("order_not_cancellable");
  transaction.commit();
  return nlohmann::json{{"id", rows[0]["order_id"].as<std::string>()},
                        {"symbol", rows[0]["symbol"].as<std::string>()},
                        {"status", rows[0]["status"].as<std::string>()}};
}

std::map<std::string, std::size_t> PostgresRepository::pending_order_symbol_counts() const {
  std::scoped_lock lock(mutex_);
  if (!healthy()) throw std::runtime_error("database_unavailable");
  pqxx::read_transaction transaction(*connection_);
  const auto rows = transaction.exec(
      "SELECT i.symbol, count(*)::bigint AS pending_count FROM orders o "
      "JOIN instruments i ON i.instrument_id = o.instrument_id "
      "WHERE o.status IN ('NEW', 'ACCEPTED', 'PARTIALLY_FILLED') GROUP BY i.symbol ORDER BY i.symbol");
  std::map<std::string, std::size_t> counts;
  for (const auto& row : rows) {
    counts.emplace(row["symbol"].as<std::string>(), row["pending_count"].as<std::size_t>());
  }
  return counts;
}

}  // namespace simtrade::database
