# Phase 1 market-data foundation

## Scope

Phase 1 establishes a searchable 30-stock Alpaca instrument catalogue, one shared Alpaca IEX WebSocket connection, Redis-backed latest-event caching, and selective backend-to-browser fan-out.

## Runtime data flow

```text
Alpaca Assets REST API ──> InstrumentCatalogue ──> Redis catalogue cache

Browser /market ──> MarketDataHub ──> one AlpacaMarketDataStream
                          ^                     │
                          │                     v
                          └──── normalized events + Redis latest-event cache
```

`main.cpp` creates exactly one `AlpacaMarketDataStream` for the backend process. Every browser connection registers with `MarketDataHub`. The hub reference-counts symbol interest across browsers and asks the shared upstream stream to subscribe only when the first browser requests a symbol. It unsubscribes when the last interested browser releases it.

The shared Alpaca connection:

- authenticates with backend-only credentials;
- subscribes to trades, quotes, minute bars, and updated bars;
- rejects duplicate symbols and the thirty-first distinct symbol by default;
- records Alpaca subscription acknowledgements;
- retains the desired symbol set across disconnects;
- reconnects with exponential backoff and jitter;
- pings the connection and reconnects when it becomes stale;
- stops cleanly on `SIGINT` or `SIGTERM`.

## Instrument catalogue

`InstrumentCatalogue` requests active `us_equity` assets from Alpaca’s `/v2/assets` endpoint at startup and on the configured interval, then retains only the supported 30-stock universe. The selected records keep symbol, name, exchange, asset class, active, tradable, and fractionable flags. Only the selected catalogue is cached at:

```text
market:instruments:catalogue
```

If Redis already contains a catalogue, it is loaded immediately while the startup refresh runs.

Search endpoint:

```http
GET /api/v1/instruments/search?q=apple&page=1&limit=20
```

Search is case-insensitive across symbol and company name. `page` starts at 1 and `limit` is capped at 100.

The matching runtime uses two independent FIFO workers. Worker 1 owns `NVDA, AAPL, MSFT, TSLA, AMZN, GOOGL, META, AVGO, AMD, MU, ORCL, PLTR, NFLX, WMT, COST`; worker 2 owns `HD, JPM, BAC, V, MA, LLY, JNJ, UNH, ABBV, XOM, CVX, GE, CAT, BA, PG`.

## Browser WebSocket protocol

Connect to:

```text
ws://localhost:8080/market
```

Subscribe or unsubscribe with:

```json
{"action":"subscribe","symbols":["AAPL"]}
{"action":"unsubscribe","symbols":["AAPL"]}
```

The backend sends cached quote, trade, bar, and status events immediately after subscription, followed by live normalized events. Updates are sent only to clients interested in the event’s symbol.

## Normalized events and Redis keys

Every public event contains `type`, `symbol`, Alpaca event `timestamp`, backend `cachedAt`, `source`, and `live`.

Latest events are stored under:

```text
market:quote:{symbol}
market:trade:{symbol}
market:bar:1m:{symbol}
market:status:{symbol}
```

## Health endpoint

```http
GET /api/v1/market-data/health
```

Market clock endpoint:

```text
GET /api/v1/market/clock
```

The endpoint uses Alpaca's authenticated market clock, returns `OPEN`, `PRE_OPEN`, or `CLOSED`, and normalizes `timestamp`, `nextOpen`, and `nextClose` to UTC. `PRE_OPEN` is the US-equity 04:00–09:30 America/New_York window; Alpaca's next-open value accounts for weekends and exchange holidays.

The response reports connection state, feed, confirmed and maximum symbol counts, catalogue readiness and size, last message time, and reconnect count.

## Configuration

See `.env.example`. Alpaca credentials never use `NEXT_PUBLIC_` names and are never sent to the browser.

The Assets API is served by Alpaca’s trading REST host, so `ALPACA_TRADING_REST_URL` is used for catalogue synchronization. Market quotes continue to use `ALPACA_DATA_REST_URL`, while streaming uses `ALPACA_DATA_WS_URL`.

## Tests

The `simtrade-market-data-tests` CTest target uses mocked JSON and in-memory fakes. It covers asset filtering, case-insensitive search and pagination, authentication payloads, duplicate prevention, the 30-symbol cap, subscription acknowledgements, normalized event/cache contracts, desired-state retention after disconnect, cached delivery, and symbol-specific frontend routing. No real credentials are required.

## Current Phase 1 limitations

- Subscription capacity is a hard limit; there is no eviction or waiting queue.
- The dashboard consumes latest quote/trade events. Historical chart backfill remains the existing simulated chart series.
- The selected market-data catalogue is cached in Redis; the same 30-stock tradable universe and its worker assignment are persisted in PostgreSQL.
- A backend process owns one Alpaca stream. Running multiple backend replicas would create one stream per process and requires cross-instance coordination in a later phase.
