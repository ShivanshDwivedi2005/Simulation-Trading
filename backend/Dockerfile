FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    libboost-system-dev libpqxx-dev libhiredis-dev libcurl4-openssl-dev libssl-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel 2 \
    && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libboost-system1.74.0 libpqxx-6.4 libhiredis0.14 libcurl4 libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/simtrade-api /usr/local/bin/simtrade-api
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/simtrade-api"]
