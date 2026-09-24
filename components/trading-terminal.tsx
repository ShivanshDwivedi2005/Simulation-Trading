"use client";

import {
  Activity,
  AreaChart,
  Bell,
  CandlestickChart,
  ChevronDown,
  CircleDollarSign,
  LineChart,
  Menu,
  Pause,
  Play,
  Search,
  ShieldCheck,
  WalletCards,
  X,
} from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";

type SymbolKey = string;
type Candle = { time: string; open: number; high: number; low: number; close: number; volume: number };
type Position = { symbol: SymbolKey; quantity: number; averagePrice: number };
type Instrument = { name: string; exchange: string; price: number; change: number; bid: number; ask: number };
type CatalogueInstrument = { symbol: string; name: string; exchange: string; tradable: boolean; fractionable: boolean };
type MarketDataStatus = "CONNECTING" | "LIVE" | "QUEUED" | "SNAPSHOT" | "STALE" | "UNAVAILABLE" | "ERROR";
type MarketDataState = {
  status: MarketDataStatus;
  message: string;
  queuePosition: number | null;
  lastUpdatedAt: string | null;
  source: string | null;
  stale: boolean;
};
type Order = {
  id: string;
  symbol: SymbolKey;
  side: "BUY" | "SELL";
  type: string;
  quantity: number;
  price: number;
  status: "FILLED" | "ACCEPTED" | "CANCELLED";
  time: string;
};

const fallbackInstruments: Record<SymbolKey, Instrument> = {
  AAPL: { name: "Apple Inc.", exchange: "NASDAQ", price: 227.16, change: 1.42, bid: 227.14, ask: 227.18 },
  NVDA: { name: "NVIDIA Corp.", exchange: "NASDAQ", price: 141.22, change: 2.84, bid: 141.2, ask: 141.25 },
  MSFT: { name: "Microsoft Corp.", exchange: "NASDAQ", price: 515.73, change: -0.38, bid: 515.68, ask: 515.78 },
  GOOGL: { name: "Alphabet Class A", exchange: "NASDAQ", price: 252.31, change: 0.76, bid: 252.27, ask: 252.35 },
  AMZN: { name: "Amazon.com Inc.", exchange: "NASDAQ", price: 231.44, change: -1.12, bid: 231.4, ask: 231.48 },
};

const API_URL = process.env.NEXT_PUBLIC_API_URL ?? "http://localhost:8080";
const WS_URL = process.env.NEXT_PUBLIC_WS_URL ?? "ws://localhost:8080/market";
const MARKET_TIME_ZONE = "America/New_York";

const intervals = ["1m", "3m", "5m", "15m", "30m", "1h", "4h", "1D", "1W"];
const marketDataStatuses = new Set<MarketDataStatus>([
  "CONNECTING", "LIVE", "QUEUED", "SNAPSHOT", "STALE", "UNAVAILABLE", "ERROR",
]);

function buildCandles(base: number, seed: number): Candle[] {
  let previous = base - 3.8;
  return Array.from({ length: 54 }, (_, index) => {
    const drift = Math.sin((index + seed) * 0.67) * 1.15 + Math.cos((index + seed) * 0.23) * 0.48 + 0.08;
    const open = previous;
    const close = Math.max(1, open + drift);
    const high = Math.max(open, close) + 0.42 + Math.abs(Math.sin(index * 1.7)) * 0.62;
    const low = Math.min(open, close) - 0.36 - Math.abs(Math.cos(index * 1.33)) * 0.55;
    previous = close;
    return {
      time: `${9 + Math.floor((30 + index * 5) / 60)}:${String((30 + index * 5) % 60).padStart(2, "0")}`,
      open,
      high,
      low,
      close,
      volume: 180_000 + ((index * 83_117 + seed * 42_013) % 720_000),
    };
  });
}

const candleSets: Record<SymbolKey, Candle[]> = {
  AAPL: buildCandles(227.16, 3),
  NVDA: buildCandles(141.22, 7),
  MSFT: buildCandles(515.73, 11),
  GOOGL: buildCandles(252.31, 17),
  AMZN: buildCandles(231.44, 23),
};

const dynamicCandleSets = new Map<string, Candle[]>();

function candlesFor(symbol: string, price: number) {
  const existing = dynamicCandleSets.get(symbol);
  if (existing) return existing;
  if (candleSets[symbol]) return candleSets[symbol];
  const seed = Array.from(symbol).reduce((sum, character) => sum + character.charCodeAt(0), 0);
  const candles = buildCandles(price > 0 ? price : 100, seed);
  dynamicCandleSets.set(symbol, candles);
  return candles;
}

function mergeHistoricalBars(symbol: string, rawBars: unknown[]) {
  const byTimestamp = new Map<string, Candle>();
  for (const raw of rawBars) {
    if (!raw || typeof raw !== "object") continue;
    const bar = raw as { t?: string; o?: number; h?: number; l?: number; c?: number; v?: number };
    if (!bar.t || ![bar.o, bar.h, bar.l, bar.c, bar.v].every((value) => Number.isFinite(Number(value)))) continue;
    const timestamp = new Date(bar.t);
    byTimestamp.set(bar.t, {
      time: Number.isNaN(timestamp.getTime())
        ? bar.t
        : timestamp.toLocaleTimeString("en-US", {
          hour: "2-digit",
          minute: "2-digit",
          hour12: false,
          timeZone: MARKET_TIME_ZONE,
        }),
      open: Number(bar.o),
      high: Number(bar.h),
      low: Number(bar.l),
      close: Number(bar.c),
      volume: Number(bar.v),
    });
  }
  const merged = Array.from(byTimestamp.values()).slice(-200);
  if (merged.length > 1) dynamicCandleSets.set(symbol, merged);
}

function lastUpdateLabel(timestamp: string | null) {
  if (!timestamp) return "No update yet";
  const value = new Date(timestamp);
  if (Number.isNaN(value.getTime())) return "Update time unavailable";
  return `Updated ${value.toLocaleString("en-US", {
    month: "short",
    day: "numeric",
    hour: "numeric",
    minute: "2-digit",
    second: "2-digit",
    timeZone: MARKET_TIME_ZONE,
    timeZoneName: "short",
  })}`;
}

const money = new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" });
const number = new Intl.NumberFormat("en-US", { maximumFractionDigits: 2 });

function PriceChart({ symbol, chartType, paused, price }: { symbol: SymbolKey; chartType: string; paused: boolean; price: number }) {
  const [cursor, setCursor] = useState<{ x: number; y: number; index: number; price: number; locked: boolean } | null>(null);
  const candles = candlesFor(symbol, price);
  const width = 960;
  const height = 350;
  const pad = { top: 18, right: 64, bottom: 42, left: 16 };
  const values = candles.flatMap((item) => [item.high, item.low]);
  const min = Math.min(...values) - 0.8;
  const max = Math.max(...values) + 0.8;
  const innerWidth = width - pad.left - pad.right;
  const innerHeight = height - pad.top - pad.bottom;
  const x = (index: number) => pad.left + (index / (candles.length - 1)) * innerWidth;
  const y = (value: number) => pad.top + ((max - value) / (max - min)) * innerHeight;
  const points = candles.map((item, index) => `${x(index)},${y(item.close)}`).join(" ");
  const areaPoints = `${pad.left},${height - pad.bottom} ${points} ${width - pad.right},${height - pad.bottom}`;
  const gridValues = Array.from({ length: 5 }, (_, index) => min + ((max - min) * index) / 4).reverse();
  const last = candles.at(-1)!;
  const tickIndexes = Array.from(new Set([0, 0.25, 0.5, 0.75, 1].map((position) => Math.round(position * (candles.length - 1)))));
  const hasHistoricalData = dynamicCandleSets.has(symbol);

  function cursorFromPoint(clientX: number, clientY: number, locked: boolean, target: SVGSVGElement) {
    const bounds = target.getBoundingClientRect();
    const svgX = ((clientX - bounds.left) / bounds.width) * width;
    const svgY = ((clientY - bounds.top) / bounds.height) * height;
    const cursorX = Math.min(width - pad.right, Math.max(pad.left, svgX));
    const cursorY = Math.min(height - pad.bottom, Math.max(pad.top, svgY));
    const index = Math.min(
      candles.length - 1,
      Math.max(0, Math.round(((cursorX - pad.left) / innerWidth) * (candles.length - 1))),
    );
    const cursorPrice = max - ((cursorY - pad.top) / innerHeight) * (max - min);
    setCursor({ x: cursorX, y: cursorY, index, price: cursorPrice, locked });
  }

  function handleChartKeyDown(event: React.KeyboardEvent<SVGSVGElement>) {
    if (event.key === "Escape") {
      setCursor(null);
      return;
    }
    if (!["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Enter", " "].includes(event.key)) return;
    event.preventDefault();
    const currentIndex = cursor?.index ?? candles.length - 1;
    const currentPrice = cursor?.price ?? candles[currentIndex].close;
    const nextIndex = event.key === "ArrowLeft"
      ? Math.max(0, currentIndex - 1)
      : event.key === "ArrowRight"
        ? Math.min(candles.length - 1, currentIndex + 1)
        : currentIndex;
    const priceStep = (max - min) / 100;
    const nextPrice = event.key === "ArrowUp"
      ? Math.min(max, currentPrice + priceStep)
      : event.key === "ArrowDown"
        ? Math.max(min, currentPrice - priceStep)
        : currentPrice;
    setCursor({ x: x(nextIndex), y: y(nextPrice), index: nextIndex, price: nextPrice, locked: true });
  }

  return (
    <div className="chart-shell" aria-label={`${symbol} price chart. Last price ${money.format(price)}. ${paused ? "Live updates paused." : "Live updates active."}`}>
      <div className="chart-context">
        <span>{hasHistoricalData ? "Historical market data" : "Simulated sample data"} · Times shown in ET</span>
        <span>Move to inspect · Click to pin · Esc to clear</span>
      </div>
      <p className="sr-only">{symbol} intraday {chartType.toLowerCase()} chart with {candles.length} OHLC bars. Session low {money.format(min + 0.8)}, session high {money.format(max - 0.8)}. Use arrow keys to inspect time and price coordinates.</p>
      <svg
        className="price-chart"
        viewBox={`0 0 ${width} ${height}`}
        role="img"
        tabIndex={0}
        aria-label={`${symbol} intraday ${chartType.toLowerCase()} price chart. Interactive crosshair available.`}
        aria-describedby="chart-cursor-readout"
        onPointerMove={(event) => {
          if (!cursor?.locked) cursorFromPoint(event.clientX, event.clientY, false, event.currentTarget);
        }}
        onPointerDown={(event) => {
          event.currentTarget.focus();
          cursorFromPoint(event.clientX, event.clientY, true, event.currentTarget);
        }}
        onPointerLeave={() => setCursor((current) => current?.locked ? current : null)}
        onKeyDown={handleChartKeyDown}
      >
        <defs>
          <linearGradient id="area-fill" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stopColor="#38bdf8" stopOpacity="0.3" />
            <stop offset="100%" stopColor="#38bdf8" stopOpacity="0" />
          </linearGradient>
        </defs>
        {gridValues.map((value) => (
          <g key={value}>
            <line x1={pad.left} x2={width - pad.right} y1={y(value)} y2={y(value)} className="chart-grid" />
            <text x={width - pad.right + 10} y={y(value) + 4} className="chart-axis">{value.toFixed(2)}</text>
          </g>
        ))}
        {tickIndexes.map((index) => (
          <text key={index} x={x(index)} y={height - 14} textAnchor={index === 0 ? "start" : index === candles.length - 1 ? "end" : "middle"} className="chart-axis">
            {candles[index].time}
          </text>
        ))}
        {chartType === "Candles" && candles.map((item, index) => {
          const rising = item.close >= item.open;
          const candleWidth = Math.max(4, innerWidth / candles.length - 5);
          return (
            <g key={`${item.time}-${index}`} className={rising ? "candle-positive" : "candle-negative"}>
              <line x1={x(index)} x2={x(index)} y1={y(item.high)} y2={y(item.low)} />
              <rect x={x(index) - candleWidth / 2} y={y(Math.max(item.open, item.close))} width={candleWidth} height={Math.max(2, Math.abs(y(item.open) - y(item.close)))} rx="1" />
            </g>
          );
        })}
        {chartType === "Area" && <polygon points={areaPoints} fill="url(#area-fill)" />}
        {chartType !== "Candles" && <polyline points={points} className="price-line" />}
        <line x1={pad.left} x2={width - pad.right} y1={y(last.close)} y2={y(last.close)} className="last-price-line" />
        <rect x={width - pad.right} y={y(last.close) - 11} width="58" height="22" rx="4" className="last-price-label" />
        <text x={width - 10} y={y(last.close) + 4} textAnchor="end" className="last-price-text">{price.toFixed(2)}</text>
        {cursor && <g className="chart-crosshair" aria-hidden="true">
          <line x1={cursor.x} x2={cursor.x} y1={pad.top} y2={height - pad.bottom} />
          <line x1={pad.left} x2={width - pad.right} y1={cursor.y} y2={cursor.y} />
          <circle cx={cursor.x} cy={cursor.y} r="4" />
          <rect x={width - pad.right} y={cursor.y - 11} width="64" height="22" rx="4" />
          <text x={width - 5} y={cursor.y + 4} textAnchor="end">{cursor.price.toFixed(2)}</text>
          <rect x={Math.min(width - pad.right - 70, Math.max(pad.left, cursor.x - 35))} y={height - pad.bottom + 7} width="70" height="22" rx="4" />
          <text x={Math.min(width - pad.right - 35, Math.max(pad.left + 35, cursor.x))} y={height - pad.bottom + 22} textAnchor="middle">{candles[cursor.index].time} ET</text>
        </g>}
      </svg>
      <output id="chart-cursor-readout" className="sr-only" aria-live="polite">
        {cursor ? `${candles[cursor.index].time} Eastern Time, price ${money.format(cursor.price)}${cursor.locked ? ", crosshair pinned" : ""}.` : "No chart coordinate selected."}
      </output>
      <details className="chart-data-table">
        <summary>View latest OHLC data ({hasHistoricalData ? "ET" : "simulated ET sample"})</summary>
        <div className="table-scroll">
          <table>
            <thead><tr><th>Time</th><th>Open</th><th>High</th><th>Low</th><th>Close</th><th>Volume</th></tr></thead>
            <tbody>{candles.slice(-5).reverse().map((item) => <tr key={item.time}><td>{item.time}</td><td>{item.open.toFixed(2)}</td><td>{item.high.toFixed(2)}</td><td>{item.low.toFixed(2)}</td><td>{item.close.toFixed(2)}</td><td>{number.format(item.volume)}</td></tr>)}</tbody>
          </table>
        </div>
      </details>
    </div>
  );
}

type TradingTerminalProps = {
  userName?: string;
  accessToken: string;
  onSignOut?: () => void;
};

export default function TradingTerminal({ userName = "Trader", accessToken, onSignOut }: TradingTerminalProps) {
  const [symbol, setSymbol] = useState<SymbolKey>("AAPL");
  const [interval, setIntervalValue] = useState("5m");
  const [chartType, setChartType] = useState("Candles");
  const [side, setSide] = useState<"BUY" | "SELL">("BUY");
  const [orderType, setOrderType] = useState("MARKET");
  const [quantity, setQuantity] = useState(10);
  const [limitPrice, setLimitPrice] = useState(226.8);
  const [stopPrice, setStopPrice] = useState(228);
  const [cash, setCash] = useState(100_000);
  const [positions, setPositions] = useState<Position[]>([]);
  const [orders, setOrders] = useState<Order[]>([]);
  const [orderSubmitting, setOrderSubmitting] = useState(false);
  const [activeTable, setActiveTable] = useState<"positions" | "orders">("positions");
  const [paused, setPaused] = useState(false);
  const [notice, setNotice] = useState("");
  const [mobileWatchlist, setMobileWatchlist] = useState(false);
  const [instruments, setInstruments] = useState(fallbackInstruments);
  const [marketData, setMarketData] = useState<MarketDataState>({
    status: "CONNECTING",
    message: "Connecting to Alpaca…",
    queuePosition: null,
    lastUpdatedAt: null,
    source: null,
    stale: false,
  });
  const [, setChartRevision] = useState(0);
  const [instrumentQuery, setInstrumentQuery] = useState("AAPL · Apple Inc.");
  const [searchResults, setSearchResults] = useState<CatalogueInstrument[]>([]);
  const [searchLoading, setSearchLoading] = useState(false);
  const [searchOpen, setSearchOpen] = useState(false);
  const searchInputRef = useRef<HTMLInputElement>(null);
  const socketRef = useRef<WebSocket | null>(null);
  const marketDataStatusRef = useRef<MarketDataStatus>("CONNECTING");
  const subscribedSymbolRef = useRef(symbol);
  const activeSymbolRef = useRef(symbol);
  const pausedRef = useRef(paused);
  const displayedMarketDataStatus = paused ? "Quote updates paused" : marketData.message;
  const marketDataTone = marketData.status.toLowerCase();

  const quote = instruments[symbol];
  const positionsValue = useMemo(
    () => positions.reduce((sum, position) => sum + (instruments[position.symbol]?.price ?? position.averagePrice) * position.quantity, 0),
    [instruments, positions],
  );
  const unrealized = useMemo(
    () => positions.reduce((sum, position) => sum + ((instruments[position.symbol]?.price ?? position.averagePrice) - position.averagePrice) * position.quantity, 0),
    [instruments, positions],
  );
  const equity = cash + positionsValue;
  const estimatedPrice = orderType === "MARKET" ? (side === "BUY" ? quote.ask : quote.bid) : limitPrice;

  useEffect(() => {
    if (!notice) return;
    const timeout = window.setTimeout(() => setNotice(""), 4200);
    return () => window.clearTimeout(timeout);
  }, [notice]);

  useEffect(() => {
    const controller = new AbortController();
    async function loadPortfolio() {
      try {
        const response = await fetch(`${API_URL}/api/v1/portfolio`, {
          headers: { Authorization: `Bearer ${accessToken}` },
          signal: controller.signal,
        });
        const result = await response.json() as {
          cash?: number;
          positions?: Array<{ symbol: string; quantity: number; average_price: number }>;
          orders?: Array<{ id: string; symbol: string; side: "BUY" | "SELL"; type: string; quantity: number; price: number; status: Order["status"]; created_at: string }>;
          error?: string;
        };
        if (!response.ok) throw new Error(result.error ?? "portfolio_request_failed");
        setCash(Number(result.cash ?? 0));
        const loadedPositions = result.positions ?? [];
        const loadedOrders = result.orders ?? [];
        setInstruments((current) => {
          const next = { ...current };
          for (const item of loadedPositions) {
            if (!next[item.symbol]) next[item.symbol] = { name: item.symbol, exchange: "US", price: Number(item.average_price), change: 0, bid: Number(item.average_price), ask: Number(item.average_price) };
          }
          for (const item of loadedOrders) {
            if (!next[item.symbol]) next[item.symbol] = { name: item.symbol, exchange: "US", price: Number(item.price), change: 0, bid: Number(item.price), ask: Number(item.price) };
          }
          return next;
        });
        setPositions(loadedPositions.map((item) => ({
          symbol: item.symbol,
          quantity: Number(item.quantity),
          averagePrice: Number(item.average_price),
        })));
        setOrders(loadedOrders.map((item) => ({
          id: item.id,
          symbol: item.symbol,
          side: item.side,
          type: item.type,
          quantity: Number(item.quantity),
          price: Number(item.price),
          status: item.status,
          time: new Date(item.created_at).toLocaleTimeString("en-US", { hour12: false }),
        })));
      } catch (error) {
        if (error instanceof DOMException && error.name === "AbortError") return;
        setNotice("Portfolio could not be loaded. Please sign in again if this continues.");
      }
    }
    void loadPortfolio();
    return () => controller.abort();
  }, [accessToken]);

  useEffect(() => {
    const controller = new AbortController();
    async function loadQuote() {
      try {
        const response = await fetch(`${API_URL}/api/v1/market/${symbol}/quote`, { signal: controller.signal });
        const result = await response.json() as {
          last?: number;
          bid?: number;
          ask?: number;
          feed?: string;
          message?: string;
          timestamp?: string;
          source?: string;
          stale?: boolean;
        };
        if (!response.ok) throw new Error(result.message ?? "quote_request_failed");
        setInstruments((current) => ({
          ...current,
          [symbol]: {
            ...current[symbol],
            price: Number(result.last),
            bid: Number(result.bid),
            ask: Number(result.ask),
          },
        }));
        setMarketData((current) => ({
          ...current,
          status: current.status === "LIVE" || current.status === "QUEUED"
            ? current.status
            : result.stale ? "STALE" : "SNAPSHOT",
          message: current.status === "LIVE" || current.status === "QUEUED"
            ? current.message
            : result.stale ? "Latest snapshot is stale." : "Latest Alpaca snapshot is shown.",
          lastUpdatedAt: result.timestamp ?? current.lastUpdatedAt,
          source: result.source ?? "alpaca_rest_snapshot",
          stale: Boolean(result.stale),
        }));
      } catch (error) {
        if (error instanceof DOMException && error.name === "AbortError") return;
        setMarketData((current) => current.status === "LIVE" || current.status === "QUEUED"
          ? current
          : { ...current, status: "UNAVAILABLE", message: "Alpaca quote unavailable" });
      }
    }
    void loadQuote();
    return () => controller.abort();
  }, [symbol]);

  useEffect(() => {
    const query = instrumentQuery.trim();
    if (!searchOpen || query.length < 1 || query === `${symbol} · ${quote.name}`) {
      return;
    }
    const controller = new AbortController();
    const timeout = window.setTimeout(async () => {
      try {
        const response = await fetch(`${API_URL}/api/v1/instruments/search?q=${encodeURIComponent(query)}&page=1&limit=20`, {
          signal: controller.signal,
        });
        const result = await response.json() as { instruments?: CatalogueInstrument[] };
        if (!response.ok) throw new Error("instrument_search_failed");
        setSearchResults(result.instruments ?? []);
      } catch (error) {
        if (error instanceof DOMException && error.name === "AbortError") return;
        setSearchResults([]);
      } finally {
        if (!controller.signal.aborted) setSearchLoading(false);
      }
    }, 220);
    return () => {
      controller.abort();
      window.clearTimeout(timeout);
    };
  }, [instrumentQuery, quote.name, searchOpen, symbol]);

  useEffect(() => {
    activeSymbolRef.current = symbol;
    pausedRef.current = paused;
  }, [paused, symbol]);

  useEffect(() => {
    marketDataStatusRef.current = marketData.status;
  }, [marketData.status]);

  useEffect(() => {
    let stopped = false;
    let reconnectTimer = 0;
    let reconnectDelay = 1_000;

    function connect() {
      if (stopped) return;
      const socket = new WebSocket(WS_URL);
      socketRef.current = socket;
      setMarketData((current) => ({ ...current, status: "CONNECTING", message: "Connecting to Alpaca stream…", queuePosition: null }));
      socket.onopen = () => {
        reconnectDelay = 1_000;
        const currentSymbol = activeSymbolRef.current;
        subscribedSymbolRef.current = currentSymbol;
        socket.send(JSON.stringify({ action: "watch", symbol: currentSymbol }));
        setMarketData((current) => ({ ...current, status: "CONNECTING", message: "Requesting live market data…" }));
      };
      socket.onmessage = (message) => {
        let event: {
          type?: string;
          symbol?: string;
          bidPrice?: number;
          askPrice?: number;
          price?: number;
          close?: number;
          connected?: boolean;
          status?: string;
          live?: boolean;
          message?: string;
          queuePosition?: number | null;
          timestamp?: string;
          source?: string;
          stale?: boolean;
          dataStatus?: "SNAPSHOT" | "STALE";
          bars?: unknown[];
        };
        try {
          event = JSON.parse(String(message.data));
        } catch {
          return;
        }
        if (event.type === "error") {
          setMarketData((current) => ({ ...current, status: "ERROR", message: event.message ?? "Market stream error" }));
          return;
        }
        if (event.type === "market_data_status" && event.symbol === activeSymbolRef.current) {
          const nextStatus = marketDataStatuses.has(event.status as MarketDataStatus)
            ? event.status as MarketDataStatus
            : event.live ? "LIVE" : "ERROR";
          if (nextStatus === "LIVE" && marketDataStatusRef.current !== "LIVE") {
            setNotice(`${event.symbol} live market data is now connected.`);
          }
          marketDataStatusRef.current = nextStatus;
          setMarketData((current) => ({
              ...current,
              status: nextStatus,
              message: event.message ?? (nextStatus === "LIVE" ? "Live market data connected." : "Market data status changed."),
              queuePosition: typeof event.queuePosition === "number" ? event.queuePosition : null,
              stale: nextStatus === "STALE" ? true : nextStatus === "LIVE" ? false : current.stale,
          }));
          return;
        }
        if (event.type === "status" && event.symbol === activeSymbolRef.current) {
          if (!event.connected) {
            setMarketData((current) => ({ ...current, status: "CONNECTING", message: "Alpaca stream reconnecting…" }));
          }
          return;
        }
        if (event.type === "historicalBars" && event.symbol === activeSymbolRef.current && Array.isArray(event.bars)) {
          mergeHistoricalBars(event.symbol, event.bars);
          setChartRevision((current) => current + 1);
          return;
        }
        if (pausedRef.current || event.symbol !== activeSymbolRef.current) return;
        const fallbackStatus = event.dataStatus ?? (event.stale ? "STALE" : "SNAPSHOT");
        setMarketData((current) => ({
          ...current,
          status: event.live
            ? "LIVE"
            : current.status === "QUEUED" || current.status === "CONNECTING" || current.status === "LIVE"
              ? current.status
              : fallbackStatus,
          message: event.live
            ? "Live market data connected."
            : current.status === "QUEUED" || current.status === "CONNECTING" || current.status === "LIVE"
              ? current.message
              : event.stale ? "Cached market data is stale." : "Latest available snapshot is shown.",
          queuePosition: event.live ? null : current.queuePosition,
          lastUpdatedAt: event.timestamp ?? current.lastUpdatedAt,
          source: event.source ?? (event.live ? "alpaca_websocket" : current.source),
          stale: Boolean(event.stale),
        }));
        if (event.type === "quote") {
          setInstruments((current) => ({
            ...current,
            [event.symbol!]: {
              ...current[event.symbol!],
              bid: Number(event.bidPrice ?? current[event.symbol!].bid),
              ask: Number(event.askPrice ?? current[event.symbol!].ask),
            },
          }));
        } else if (event.type === "snapshot") {
          const nextPrice = Number(event.price);
          setInstruments((current) => ({
            ...current,
            [event.symbol!]: {
              ...current[event.symbol!],
              price: Number.isFinite(nextPrice) ? nextPrice : current[event.symbol!].price,
              bid: Number(event.bidPrice ?? current[event.symbol!].bid),
              ask: Number(event.askPrice ?? current[event.symbol!].ask),
            },
          }));
        } else if (event.type === "trade" || event.type === "bar" || event.type === "updatedBar") {
          const nextPrice = Number(event.price ?? event.close);
          if (!Number.isFinite(nextPrice)) return;
          setInstruments((current) => ({
            ...current,
            [event.symbol!]: { ...current[event.symbol!], price: nextPrice },
          }));
        }
      };
      socket.onclose = () => {
        if (socketRef.current === socket) socketRef.current = null;
        if (stopped) return;
        setMarketData((current) => ({ ...current, status: "CONNECTING", message: "Alpaca stream reconnecting…" }));
        reconnectTimer = window.setTimeout(connect, reconnectDelay);
        reconnectDelay = Math.min(15_000, reconnectDelay * 2);
      };
      socket.onerror = () => socket.close();
    }

    connect();
    return () => {
      stopped = true;
      window.clearTimeout(reconnectTimer);
      const socket = socketRef.current;
      if (socket?.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ action: "unwatch", symbol: subscribedSymbolRef.current }));
      }
      socket?.close();
      socketRef.current = null;
    };
  }, []);

  useEffect(() => {
    const socket = socketRef.current;
    const previous = subscribedSymbolRef.current;
    activeSymbolRef.current = symbol;
    if (previous === symbol) return;
    marketDataStatusRef.current = "CONNECTING";
    setMarketData({
      status: "CONNECTING",
      message: `Requesting ${symbol} market data…`,
      queuePosition: null,
      lastUpdatedAt: null,
      source: null,
      stale: false,
    });
    if (!socket || socket.readyState !== WebSocket.OPEN) return;
    socket.send(JSON.stringify({ action: "unwatch", symbol: previous }));
    socket.send(JSON.stringify({ action: "watch", symbol }));
    subscribedSymbolRef.current = symbol;
  }, [symbol]);

  function selectInstrument(nextSymbol: SymbolKey) {
    const nextQuote = instruments[nextSymbol];
    setSymbol(nextSymbol);
    setLimitPrice(Number((nextQuote.price - 0.35).toFixed(2)));
    setStopPrice(Number((nextQuote.price + 0.8).toFixed(2)));
  }

  function selectCatalogueInstrument(instrument: CatalogueInstrument) {
    setInstruments((current) => ({
      ...current,
      [instrument.symbol]: current[instrument.symbol] ?? {
        name: instrument.name,
        exchange: instrument.exchange,
        price: 0,
        change: 0,
        bid: 0,
        ask: 0,
      },
    }));
    setInstrumentQuery(`${instrument.symbol} · ${instrument.name}`);
    setSearchOpen(false);
    setSearchResults([]);
    setSearchLoading(false);
    setSymbol(instrument.symbol);
    setLimitPrice(0);
    setStopPrice(0);
  }

  async function submitOrder(event: React.FormEvent) {
    event.preventDefault();
    if (!Number.isFinite(quantity) || quantity <= 0) {
      setNotice("Enter a quantity greater than zero.");
      return;
    }
    setOrderSubmitting(true);
    try {
      const response = await fetch(`${API_URL}/api/v1/orders`, {
        method: "POST",
        headers: { Authorization: `Bearer ${accessToken}`, "Content-Type": "application/json" },
        body: JSON.stringify({
          symbol,
          side,
          type: orderType,
          quantity,
          ...(orderType === "LIMIT" || orderType === "STOP_LIMIT" ? { limit_price: limitPrice } : {}),
          ...(orderType === "STOP" || orderType === "STOP_LIMIT" ? { stop_price: stopPrice } : {}),
        }),
      });
      const result = await response.json() as {
        id?: string;
        symbol?: SymbolKey;
        side?: "BUY" | "SELL";
        type?: string;
        quantity?: number;
        price?: number;
        status?: Order["status"];
        created_at?: string;
        message?: string;
        error?: string;
      };
      if (!response.ok) throw new Error((result.message ?? result.error ?? "Order rejected").replaceAll("_", " "));
      const newOrder: Order = {
        id: result.id ?? "ORDER",
        symbol: result.symbol ?? symbol,
        side: result.side ?? side,
        type: result.type ?? orderType,
        quantity: Number(result.quantity ?? quantity),
        price: Number(result.price ?? estimatedPrice),
        status: result.status ?? "ACCEPTED",
        time: result.created_at ? new Date(result.created_at).toLocaleTimeString("en-US", { hour12: false }) : new Date().toLocaleTimeString("en-US", { hour12: false }),
      };
      setOrders((current) => [newOrder, ...current]);
      setActiveTable("orders");

      const portfolioResponse = await fetch(`${API_URL}/api/v1/portfolio`, { headers: { Authorization: `Bearer ${accessToken}` } });
      if (portfolioResponse.ok) {
        const portfolio = await portfolioResponse.json() as { cash: number; positions: Array<{ symbol: string; quantity: number; average_price: number }> };
        setCash(Number(portfolio.cash));
        setPositions(portfolio.positions.map((item) => ({ symbol: item.symbol, quantity: Number(item.quantity), averagePrice: Number(item.average_price) })));
      }
      setNotice(`${newOrder.side} ${newOrder.quantity} ${newOrder.symbol} ${newOrder.status === "FILLED" ? `filled at ${money.format(newOrder.price)}` : "accepted"}.`);
    } catch (error) {
      setNotice(error instanceof Error ? `Order rejected: ${error.message}.` : "Order rejected by the backend.");
    } finally {
      setOrderSubmitting(false);
    }
  }

  async function cancelOrder(id: string) {
    try {
      const response = await fetch(`${API_URL}/api/v1/orders/${encodeURIComponent(id)}`, {
        method: "DELETE",
        headers: { Authorization: `Bearer ${accessToken}` },
      });
      const result = await response.json() as { status?: Order["status"]; message?: string; error?: string };
      if (!response.ok) throw new Error((result.message ?? result.error ?? "Order cancellation failed").replaceAll("_", " "));
      setOrders((current) => current.map((order) => order.id === id ? { ...order, status: result.status ?? "CANCELLED" } : order));
      setNotice(`${id.slice(0, 8).toUpperCase()} cancelled.`);
    } catch (error) {
      setNotice(error instanceof Error ? `Cancellation failed: ${error.message}.` : "Order cancellation failed.");
    }
  }

  return (
    <main className="terminal-shell">
      <header className="topbar">
        <div className="brand-block">
          <button className="icon-button mobile-only" aria-label="Open watchlist" onClick={() => setMobileWatchlist(true)}><Menu aria-hidden="true" /></button>
          <div className="brand-mark" aria-hidden="true"><Activity /></div>
          <div><strong>SIMTRADE</strong><span>US SIMULATION</span></div>
        </div>
        <label className="instrument-search" onBlur={(event) => {
          if (!event.currentTarget.contains(event.relatedTarget)) setSearchOpen(false);
        }}>
          <Search aria-hidden="true" />
          <span className="sr-only">Search instruments</span>
          <input
            ref={searchInputRef}
            type="search"
            role="combobox"
            aria-label="Search all supported US equities and ETFs"
            aria-autocomplete="list"
            aria-controls="instrument-results"
            aria-expanded={searchOpen}
            value={instrumentQuery}
            onFocus={() => setSearchOpen(true)}
            onChange={(event) => { setInstrumentQuery(event.target.value); setSearchResults([]); setSearchLoading(true); setSearchOpen(true); }}
            onKeyDown={(event) => { if (event.key === "Escape") setSearchOpen(false); }}
            placeholder="Search symbol or company"
          />
          <span className="shortcut" aria-hidden="true">/</span>
          {searchOpen && instrumentQuery.trim() && instrumentQuery.trim() !== `${symbol} · ${quote.name}` && <div id="instrument-results" className="instrument-results" role="listbox" aria-label="Instrument search results">
            {searchLoading && <p role="status">Searching instruments…</p>}
            {!searchLoading && searchResults.map((instrument) => <button key={instrument.symbol} type="button" role="option" aria-selected="false" onClick={() => selectCatalogueInstrument(instrument)}>
              <span><strong>{instrument.symbol}</strong><small>{instrument.exchange}</small></span>
              <span>{instrument.name}</span>
            </button>)}
            {!searchLoading && searchResults.length === 0 && <p>No matching instruments. Try a symbol or company name.</p>}
          </div>}
        </label>
        <div className="topbar-actions">
          <span className={`market-status ${marketDataTone}`} role="status" aria-atomic="true">
            <i aria-hidden="true" />
            <b>{marketData.status}{marketData.status === "QUEUED" && marketData.queuePosition ? ` · #${marketData.queuePosition}` : ""}</b>
          </span>
          <button className="icon-button" aria-label="Notifications"><Bell aria-hidden="true" /><span className="notification-dot" /></button>
          <button className="account-button" onClick={onSignOut} aria-label={`Sign out ${userName}`} title="Sign out"><span>{userName.split(/\s+/).map((part) => part[0]).join("").slice(0, 2).toUpperCase() || "ST"}</span><div><strong>{userName}</strong><small>Demo account · Sign out</small></div><ChevronDown aria-hidden="true" /></button>
        </div>
      </header>

      <div className="workspace-grid">
        <aside className={`watchlist-panel ${mobileWatchlist ? "mobile-open" : ""}`}>
          <div className="panel-heading"><div><small>MARKET</small><h2>Watchlist</h2></div><button className="icon-button mobile-only" aria-label="Close watchlist" onClick={() => setMobileWatchlist(false)}><X aria-hidden="true" /></button></div>
          <div className="watchlist-columns"><span>Symbol</span><span>Last</span><span>Change</span></div>
          <div className="watchlist-items">
            {(Object.keys(instruments) as SymbolKey[]).map((key) => {
              const item = instruments[key];
              return <button key={key} className={`watchlist-row ${key === symbol ? "active" : ""}`} aria-label={`${key}, ${item.name}, last price ${item.price.toFixed(2)}`} onClick={() => { selectInstrument(key); setMobileWatchlist(false); }}>
                <span><strong>{key}</strong><small title={item.name}>{item.name}</small></span>
                <span className="numeric">{item.price.toFixed(2)}</span>
                <span className={`numeric ${item.change >= 0 ? "positive" : "negative"}`}>{item.change >= 0 ? "+" : ""}{item.change.toFixed(2)}%</span>
              </button>;
            })}
          </div>
          <button className="add-symbol" onClick={() => { searchInputRef.current?.focus(); setSearchOpen(true); }}><Search aria-hidden="true" /> Add symbol</button>
          <div className={`data-source ${marketDataTone}`}><ShieldCheck aria-hidden="true" /><span><strong>Alpaca market data · {marketData.status}</strong><small>{displayedMarketDataStatus}</small><small>{lastUpdateLabel(marketData.lastUpdatedAt)}</small></span></div>
        </aside>

        <section className="main-workspace">
          <div className="quote-strip">
            <div className="quote-identity"><span className="instrument-avatar">{symbol.slice(0, 1)}</span><div><div><h1>{symbol}</h1><span>{quote.exchange || "US"}</span></div><p>{quote.name} · USD</p></div></div>
            <div className="quote-price"><strong>{quote.price.toFixed(2)}</strong><span className={quote.change >= 0 ? "positive" : "negative"}>{quote.change >= 0 ? "+" : ""}{(quote.price * quote.change / 100).toFixed(2)} ({quote.change >= 0 ? "+" : ""}{quote.change.toFixed(2)}%)</span></div>
            <dl className="quote-stats"><div><dt>Bid</dt><dd>{quote.bid.toFixed(2)}</dd></div><div><dt>Ask</dt><dd>{quote.ask.toFixed(2)}</dd></div><div><dt>Spread</dt><dd>{(quote.ask - quote.bid).toFixed(2)}</dd></div><div><dt>Volume</dt><dd>42.8M</dd></div></dl>
          </div>

          <div className="chart-panel">
            <div className="chart-toolbar">
              <div className="segmented-control" aria-label="Chart type">
                <button className={chartType === "Candles" ? "active" : ""} onClick={() => setChartType("Candles")}><CandlestickChart aria-hidden="true" /> <span>Candles</span></button>
                <button className={chartType === "Line" ? "active" : ""} onClick={() => setChartType("Line")}><LineChart aria-hidden="true" /> <span>Line</span></button>
                <button className={chartType === "Area" ? "active" : ""} onClick={() => setChartType("Area")}><AreaChart aria-hidden="true" /> <span>Area</span></button>
              </div>
              <div className="interval-control" aria-label="Chart timeframe">{intervals.map((item) => <button key={item} className={interval === item ? "active" : ""} onClick={() => setIntervalValue(item)}>{item}</button>)}</div>
              <button className="icon-button" aria-label={paused ? "Resume live chart" : "Pause live chart"} aria-pressed={paused} onClick={() => setPaused((current) => !current)}>{paused ? <Play aria-hidden="true" /> : <Pause aria-hidden="true" />}</button>
            </div>
            <div className="ohlc-strip">
              <span>O <b>225.84</b></span><span>H <b>228.12</b></span><span>L <b>224.92</b></span><span>C <b className="positive">227.16</b></span><span>Vol <b>642.8K</b></span>
              <span className={`market-data-detail ${marketDataTone}`}>{marketData.status}{marketData.queuePosition ? ` · Queue #${marketData.queuePosition}` : ""} · {lastUpdateLabel(marketData.lastUpdatedAt)}</span>
              {marketData.stale && <em className="stale-warning">Stale data · orders require a fresh price</em>}
              {paused && <em>Paused</em>}
            </div>
            <PriceChart key={`${symbol}-${chartType}`} symbol={symbol} chartType={chartType} paused={paused} price={quote.price} />
          </div>

          <section className="activity-panel">
            <div className="activity-tabs" role="tablist" aria-label="Account activity">
              <button role="tab" aria-selected={activeTable === "positions"} className={activeTable === "positions" ? "active" : ""} onClick={() => setActiveTable("positions")}>Positions <span>{positions.length}</span></button>
              <button role="tab" aria-selected={activeTable === "orders"} className={activeTable === "orders" ? "active" : ""} onClick={() => setActiveTable("orders")}>Orders <span>{orders.length}</span></button>
            </div>
            <div className="table-scroll">
              {activeTable === "positions" ? <table>
                <thead><tr><th>Symbol</th><th>Qty</th><th>Avg price</th><th>Last</th><th>Market value</th><th>Unrealized P&amp;L</th></tr></thead>
                <tbody>{positions.map((position) => {
                  const positionInstrument = instruments[position.symbol] ?? { name: position.symbol, exchange: "US", price: position.averagePrice, change: 0, bid: position.averagePrice, ask: position.averagePrice };
                  const last = positionInstrument.price;
                  const pnl = (last - position.averagePrice) * position.quantity;
                  return <tr key={position.symbol}><td><strong>{position.symbol}</strong><small>{positionInstrument.name}</small></td><td>{position.quantity}</td><td>{money.format(position.averagePrice)}</td><td>{money.format(last)}</td><td>{money.format(last * position.quantity)}</td><td className={pnl >= 0 ? "positive" : "negative"}>{pnl >= 0 ? "+" : ""}{money.format(pnl)}</td></tr>;
                })}</tbody>
              </table> : <table>
                <thead><tr><th>Order</th><th>Symbol</th><th>Side</th><th>Type</th><th>Qty</th><th>Price</th><th>Status</th><th><span className="sr-only">Actions</span></th></tr></thead>
                <tbody>{orders.map((order) => <tr key={order.id}><td><strong>{order.id.slice(0, 8).toUpperCase()}</strong><small>{order.time}</small></td><td>{order.symbol}</td><td className={order.side === "BUY" ? "positive" : "negative"}>{order.side}</td><td>{order.type}</td><td>{order.quantity}</td><td>{money.format(order.price)}</td><td><span className={`status-badge ${order.status.toLowerCase()}`}>{order.status}</span></td><td>{order.status === "ACCEPTED" && <button className="table-action" onClick={() => void cancelOrder(order.id)}>Cancel</button>}</td></tr>)}</tbody>
              </table>}
            </div>
          </section>
        </section>

        <aside className="trade-panel">
          <section className="account-card">
            <div className="card-title"><span><WalletCards aria-hidden="true" /> Simulated portfolio</span><small>USD</small></div>
            <strong className="equity-value">{money.format(equity)}</strong>
            <span className="equity-change positive">+{money.format(unrealized)} all time</span>
            <div className="account-metrics"><div><span>Available cash</span><strong>{money.format(cash)}</strong></div><div><span>Buying power</span><strong>{money.format(cash * 2)}</strong></div><div><span>Positions</span><strong>{money.format(positionsValue)}</strong></div></div>
          </section>

          <form className="order-ticket" onSubmit={submitOrder}>
            <div className="ticket-heading"><div><small>ORDER TICKET</small><h2>{symbol}</h2></div><span className={`live-badge ${marketDataTone}`}><i aria-hidden="true" /> {marketData.status}</span></div>
            <div className="side-toggle"><button type="button" className={side === "BUY" ? "buy active" : "buy"} onClick={() => setSide("BUY")}>Buy</button><button type="button" className={side === "SELL" ? "sell active" : "sell"} onClick={() => setSide("SELL")}>Sell</button></div>
            <label className="field-label">Order type<select value={orderType} onChange={(event) => setOrderType(event.target.value)}><option>MARKET</option><option>LIMIT</option><option>STOP</option><option>STOP_LIMIT</option></select></label>
            <label className="field-label">Quantity<div className="input-with-unit"><input type="number" min="1" step="1" value={quantity} onChange={(event) => setQuantity(Number(event.target.value))} /><span>Shares</span></div></label>
            {(orderType === "LIMIT" || orderType === "STOP_LIMIT") && <label className="field-label">Limit price<div className="input-with-unit"><span>$</span><input type="number" min="0.01" step="0.01" value={limitPrice} onChange={(event) => setLimitPrice(Number(event.target.value))} /></div></label>}
            {(orderType === "STOP" || orderType === "STOP_LIMIT") && <label className="field-label">Stop price<div className="input-with-unit"><span>$</span><input type="number" min="0.01" step="0.01" value={stopPrice} onChange={(event) => setStopPrice(Number(event.target.value))} /></div></label>}
            <div className="quick-quantity" aria-label="Quick quantity"><button type="button" onClick={() => setQuantity(1)}>1</button><button type="button" onClick={() => setQuantity(10)}>10</button><button type="button" onClick={() => setQuantity(25)}>25</button><button type="button" onClick={() => setQuantity(50)}>50</button></div>
            <dl className="order-summary"><div><dt>Estimated price</dt><dd>{money.format(estimatedPrice)}</dd></div><div><dt>Estimated total</dt><dd>{money.format(estimatedPrice * quantity)}</dd></div><div><dt>Execution</dt><dd>{orderType === "MARKET" ? (side === "BUY" ? "Ask side" : "Bid side") : "When triggered"}</dd></div></dl>
            <button className={`submit-order ${side.toLowerCase()}`} type="submit" disabled={orderSubmitting}>{orderSubmitting ? "Sending order…" : `Place ${side.toLowerCase()} order`}</button>
            <p className="ticket-note"><ShieldCheck aria-hidden="true" /> No real funds are used. Orders are executed by the simulation engine.</p>
          </form>

          <section className="risk-card"><div><CircleDollarSign aria-hidden="true" /><span><strong>Risk limits active</strong><small>Max position 25% · Daily loss 5%</small></span></div><button aria-label="View risk settings"><ChevronDown aria-hidden="true" /></button></section>
        </aside>
      </div>

      {notice && <output className="toast" aria-live="polite">{notice}</output>}
    </main>
  );
}
