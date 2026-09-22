cmake_minimum_required(VERSION 3.20)
project(simtrade_backend VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

add_executable(simtrade-api
  src/main.cpp
  src/config/Config.cpp
  src/database/PostgresRepository.cpp
  src/cache/RedisClient.cpp
  src/execution/SimulationEngine.cpp
  src/api/HttpServer.cpp
)

target_include_directories(simtrade-api PRIVATE include)
target_link_libraries(simtrade-api PRIVATE
  Boost::system
  CURL::libcurl
  nlohmann_json::nlohmann_json
  Threads::Threads
  PkgConfig::PQXX
  PkgConfig::HIREDIS
)

target_compile_options(simtrade-api PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion>
)

install(TARGETS simtrade-api RUNTIME DESTINATION bin)
