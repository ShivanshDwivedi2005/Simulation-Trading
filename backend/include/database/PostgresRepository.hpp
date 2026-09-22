#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <pqxx/connection>
#include <string>

namespace simtrade::database {

class PostgresRepository {
 public:
  explicit PostgresRepository(const std::string& connection_string);
  ~PostgresRepository();
  PostgresRepository(const PostgresRepository&) = delete;
  PostgresRepository& operator=(const PostgresRepository&) = delete;

  [[nodiscard]] bool healthy() const noexcept;
  [[nodiscard]] std::string status() const;
  [[nodiscard]] nlohmann::json register_user(const std::string& name,
                                             const std::string& email,
                                             const std::string& password,
                                             const std::string& otp,
                                             const std::string& otp_pepper,
                                             std::uint32_t otp_ttl_seconds);
  [[nodiscard]] std::optional<nlohmann::json> verify_otp(const std::string& email,
                                                        const std::string& otp,
                                                        const std::string& otp_pepper,
                                                        std::uint32_t access_token_ttl_seconds);
  [[nodiscard]] std::optional<nlohmann::json> login(const std::string& email,
                                                   const std::string& password,
                                                   std::uint32_t access_token_ttl_seconds);
  [[nodiscard]] std::optional<nlohmann::json> portfolio(const std::string& access_token) const;
  [[nodiscard]] std::optional<nlohmann::json> place_order(const std::string& access_token,
                                                          const std::string& symbol,
                                                          const std::string& side,
                                                          const std::string& order_type,
                                                          double quantity,
                                                          std::optional<double> limit_price,
                                                          std::optional<double> stop_price,
                                                          double bid,
                                                          double ask);

 private:
  std::unique_ptr<pqxx::connection> connection_;
  std::string error_;
  mutable std::mutex mutex_;
};

}  // namespace simtrade::database
