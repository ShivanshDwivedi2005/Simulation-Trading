"use client";

import {
  Activity,
  AreaChart,
  Bell,
  BookOpen,
  CandlestickChart,
  CheckCircle2,
  ChevronDown,
  CircleDollarSign,
  ClipboardList,
  LineChart,
  Menu,
  Pause,
  Play,
  Search,
  ShieldCheck,
  SlidersHorizontal,
  WalletCards,
  X,
} from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import ThemeToggle from "@/components/theme-toggle";

type SymbolKey = string;
type Candle = { timestamp?: string; time: string; sessionMinute: number; open: number; high: number; low: number; close: number; volume: number };
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
type MarketSessionStatus = "LOADING" | "OPEN" | "PRE_OPEN" | "CLOSED" | "UNAVAILABLE";
type MarketClockState = {
  status: MarketSessionStatus;
  timestamp: string | null;
  nextOpen: string | null;
  nextClose: string | null;
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
type WorkspaceView = "graph" | "orderbook" | "fills" | "positions" | "audit";

const fallbackInstruments: Record<SymbolKey, Instrument> = {
  AAPL: { name: "Apple Inc.", exchange: "NASDAQ", price: 227.16, change: 1.42, bid: 227.14, ask: 227.18 },
  NVDA: { name: "NVIDIA Corp.", exchange: "NASDAQ", price: 141.22, change: 2.84, bid: 141.2, ask: 141.25 },
  MSFT: { name: "Microsoft Corp.", exchange: "NASDAQ", price: 515.73, change: -0.38, bid: 515.68, ask: 515.78 },
  GOOGL: { name: "Alphabet Class A", exchange: "NASDAQ", price: 252.31, change: 0.76, bid: 252.27, ask: 252.35 },
  AMZN: { name: "Amazon.com Inc.", exchange: "NASDAQ", price: 231.44, change: -1.12, bid: 231.4, ask: 231.48 },
};

const API_URL = process.env.NEXT_PUBLIC_API_URL ?? "http://localhost:8080";
const WS_URL = process.env.NEXT_PUBLIC_WS_URL ?? "ws://localhost:8080/market";
const DISPLAY_TIME_ZONE = "UTC";
const EXCHANGE_TIME_ZONE = "America/New_York";
const REGULAR_SESSION_OPEN_MINUTE = 9 * 60 + 30;
const REGULAR_SESSION_CLOSE_MINUTE = 16 * 60;
const REGULAR_SESSION_MINUTES = REGULAR_SESSION_CLOSE_MINUTE - REGULAR_SESSION_OPEN_MINUTE;

const intervals = ["1m", "3m", "5m", "15m", "30m", "1h", "4h", "1D", "1W"];
const workspaceMenuItems = [
  { id: "graph", label: "Graph", description: "Interactive price chart", icon: LineChart },
  { id: "orderbook", label: "Order book", description: "Indicative market depth", icon: BookOpen },
  { id: "fills", label: "Fill book", description: "Completed executions", icon: CheckCircle2 },
  { id: "positions", label: "Positions", description: "Open holdings and P&L", icon: WalletCards },
  { id: "audit", label: "Audit trail", description: "Order event history", icon: ClipboardList },
] satisfies Array<{ id: WorkspaceView; label: string; description: string; icon: typeof LineChart }>;
const marketDataStatuses = new Set<MarketDataStatus>([
  "CONNECTING", "LIVE", "QUEUED", "SNAPSHOT", "STALE", "UNAVAILABLE", "ERROR",
]);

const exchangeSessionFormatter = new Intl.DateTimeFormat("en-US", {
  timeZone: EXCHANGE_TIME_ZONE,
  year: "numeric",
  month: "2-digit",
  day: "2-digit",
  hour: "2-digit",
  minute: "2-digit",
  hourCycle: "h23",
});

const utcSessionFormatter = new Intl.DateTimeFormat("en-US", {
  timeZone: DISPLAY_TIME_ZONE,
  year: "numeric",
  month: "2-digit",
  day: "2-digit",
  hour: "2-digit",
  minute: "2-digit",
  hourCycle: "h23",
});

function dateTimeParts(formatter: Intl.DateTimeFormat, value: Date) {
  return Object.fromEntries(
    formatter.formatToParts(value)
      .filter((part) => part.type !== "literal")
      .map((part) => [part.type, part.value]),
  );
}

function regularSessionOpenUtcMinute(value: Date) {
  const exchange = dateTimeParts(exchangeSessionFormatter, value);
  const utc = dateTimeParts(utcSessionFormatter, value);
  const exchangeDate = Date.UTC(Number(exchange.year), Number(exchange.month) - 1, Number(exchange.day));
  const utcDate = Date.UTC(Number(utc.year), Number(utc.month) - 1, Number(utc.day));
  const dateDifferenceMinutes = Math.round((exchangeDate - utcDate) / 60_000);
  const exchangeOffsetMinutes = dateDifferenceMinutes + Number(exchange.hour) * 60 + Number(exchange.minute) - (Number(utc.hour) * 60 + Number(utc.minute));
  return REGULAR_SESSION_OPEN_MINUTE - exchangeOffsetMinutes;
}

function buildCandles(base: number, seed: number): Candle[] {
  let previous = base - 3.8;
  const sessionOpenMinute = regularSessionOpenUtcMinute(new Date());
  return Array.from({ length: REGULAR_SESSION_MINUTES }, (_, index) => {
    const drift = Math.sin((index + seed) * 0.67) * 0.18 + Math.cos((index + seed) * 0.23) * 0.08 + 0.006;
    const open = previous;
    const close = Math.max(1, open + drift);
    const high = Math.max(open, close) + 0.08 + Math.abs(Math.sin(index * 1.7)) * 0.12;
    const low = Math.min(open, close) - 0.07 - Math.abs(Math.cos(index * 1.33)) * 0.11;
    previous = close;
    const sessionMinute = sessionOpenMinute + index;
    return {
      time: `${String(Math.floor(sessionMinute / 60)).padStart(2, "0")}:${String(sessionMinute % 60).padStart(2, "0")}`,
      sessionMinute,
      open,
      high,
      low,
      close,
      volume: 30_000 + ((index * 13_117 + seed * 7_013) % 120_000),
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

const marketCandleSets = new Map<string, Candle[]>();
const fallbackCandleSets = new Map<string, Candle[]>();

function marketSessionDetails(timestamp: string) {
  const value = new Date(timestamp);
  if (Number.isNaN(value.getTime())) return null;
  const exchange = dateTimeParts(exchangeSessionFormatter, value);
  const utc = dateTimeParts(utcSessionFormatter, value);
  const exchangeHour = Number(exchange.hour);
  const exchangeMinute = Number(exchange.minute);
  const utcHour = Number(utc.hour);
  const utcMinute = Number(utc.minute);
  if (!exchange.year || !exchange.month || !exchange.day || ![exchangeHour, exchangeMinute, utcHour, utcMinute].every(Number.isFinite)) return null;
  return {
    exchangeDate: `${exchange.year}-${exchange.month}-${exchange.day}`,
    exchangeMinuteOfDay: exchangeHour * 60 + exchangeMinute,
    utcMinuteOfDay: utcHour * 60 + utcMinute,
    time: `${String(utcHour).padStart(2, "0")}:${String(utcMinute).padStart(2, "0")}`,
  };
}

function candleFromMarketBar(raw: unknown): Candle | null {
  if (!raw || typeof raw !== "object") return null;
  const bar = raw as { timestamp?: string; t?: string; open?: number; o?: number; high?: number; h?: number; low?: number; l?: number; close?: number; c?: number; volume?: number; v?: number };
  const timestamp = bar.timestamp ?? bar.t;
  const open = Number(bar.open ?? bar.o);
  const high = Number(bar.high ?? bar.h);
  const low = Number(bar.low ?? bar.l);
  const close = Number(bar.close ?? bar.c);
  const volume = Number(bar.volume ?? bar.v);
  if (!timestamp || ![open, high, low, close, volume].every(Number.isFinite)) return null;
  const session = marketSessionDetails(timestamp);
  if (!session || session.exchangeMinuteOfDay < REGULAR_SESSION_OPEN_MINUTE || session.exchangeMinuteOfDay >= REGULAR_SESSION_CLOSE_MINUTE) return null;
  return { timestamp, time: session.time, sessionMinute: session.utcMinuteOfDay, open, high, low, close, volume };
}

function mergeMarketCandles(symbol: string, incoming: Candle[], incomingWins: boolean) {
  const existing = marketCandleSets.get(symbol) ?? [];
  const candidates = incomingWins ? [...existing, ...incoming] : [...incoming, ...existing];
  let latestSession = "";
  const byTimestamp = new Map<string, Candle>();
  for (const candle of candidates) {
    if (!candle.timestamp) continue;
    const session = marketSessionDetails(candle.timestamp);
    if (!session) continue;
    if (session.exchangeDate > latestSession) {
      latestSession = session.exchangeDate;
      byTimestamp.clear();
    }
    if (session.exchangeDate === latestSession) byTimestamp.set(candle.timestamp, candle);
  }
  const merged = Array.from(byTimestamp.values())
    .sort((left, right) => left.timestamp!.localeCompare(right.timestamp!))
    .slice(-REGULAR_SESSION_MINUTES);
  if (merged.length === 0) return false;
  marketCandleSets.set(symbol, merged);
  return true;
}

function candlesFor(symbol: string, price: number) {
  const existing = marketCandleSets.get(symbol);
  if (existing) return existing;
  if (candleSets[symbol]) return candleSets[symbol];
  const fallback = fallbackCandleSets.get(symbol);
  if (fallback) return fallback;
  const seed = Array.from(symbol).reduce((sum, character) => sum + character.charCodeAt(0), 0);
  const candles = buildCandles(price > 0 ? price : 100, seed);
  fallbackCandleSets.set(symbol, candles);
  return candles;
}

function mergeHistoricalBars(symbol: string, rawBars: unknown[]) {
  return mergeMarketCandles(symbol, rawBars.map(candleFromMarketBar).filter((bar): bar is Candle => bar !== null), false);
}

function mergeLiveBar(symbol: string, rawBar: unknown) {
  const candle = candleFromMarketBar(rawBar);
  return candle ? mergeMarketCandles(symbol, [candle], true) : false;
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
    timeZone: DISPLAY_TIME_ZONE,
    timeZoneName: "short",
  })}`;
}

function utcTimeLabel(timestamp: string | null) {
  if (!timestamp) return "Time unavailable";
  const value = new Date(timestamp);
  if (Number.isNaN(value.getTime())) return "Time unavailable";
  return value.toLocaleString("en-US", {
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hourCycle: "h23",
    timeZone: DISPLAY_TIME_ZONE,
    timeZoneName: "short",
  });
}

const money = new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" });
const number = new Intl.NumberFormat("en-US", { maximumFractionDigits: 2 });
const CHART_WIDTH = 960;
const CHART_HEIGHT = 350;
const CHART_PAD = { top: 18, right: 64, bottom: 42, left: 16 } as const;

function PriceChart({ symbol, chartType, paused, price }: { symbol: SymbolKey; chartType: string; paused: boolean; price: number }) {
  const [cursor, setCursor] = useState<{ x: number; y: number; minute: number; time: string; price: number; locked: boolean } | null>(null);
  const [zoom, setZoom] = useState(1);
  const chartRef = useRef<SVGSVGElement>(null);
  const chartShellRef = useRef<HTMLDivElement>(null);
  const pointerFrameRef = useRef<number | null>(null);
  const pendingPointerRef = useRef<{ clientX: number; clientY: number; target: SVGSVGElement } | null>(null);
  const allCandles = candlesFor(symbol, price);
  const maxZoom = Math.max(1, Math.min(16, allCandles.length / 24));
  const visibleCount = Math.max(24, Math.ceil(allCandles.length / zoom));
  const candles = useMemo(() => allCandles.slice(-visibleCount), [allCandles, visibleCount]);
  const { min, max } = useMemo(() => {
    const values = [price, ...candles.flatMap((item) => [item.high, item.low])];
    return { min: Math.min(...values) - 0.8, max: Math.max(...values) + 0.8 };
  }, [candles, price]);
  const innerWidth = CHART_WIDTH - CHART_PAD.left - CHART_PAD.right;
  const innerHeight = CHART_HEIGHT - CHART_PAD.top - CHART_PAD.bottom;
  const sessionOpenMinute = candles[0]?.sessionMinute ?? regularSessionOpenUtcMinute(new Date());
  const sessionCloseMinute = candles.at(-1)?.sessionMinute ?? sessionOpenMinute + REGULAR_SESSION_MINUTES;
  const visibleSessionMinutes = Math.max(1, sessionCloseMinute - sessionOpenMinute);
  const formatSessionMinute = (minute: number) => `${String(Math.floor(minute / 60)).padStart(2, "0")}:${String(minute % 60).padStart(2, "0")}`;
  const xForMinute = (minute: number) => CHART_PAD.left + ((minute - sessionOpenMinute) / visibleSessionMinutes) * innerWidth;
  const y = (value: number) => CHART_PAD.top + ((max - value) / (max - min)) * innerHeight;
  const last = candles.at(-1)!;
  const hasMarketData = marketCandleSets.has(symbol);
  const cursorPriceLabelWidth = 64;
  const cursorPriceLabelGap = 8;
  const cursorPriceLabelX = CHART_WIDTH - CHART_PAD.right - cursorPriceLabelWidth - cursorPriceLabelGap;

  const chartPlot = useMemo(() => {
    const yForValue = (value: number) => CHART_PAD.top + ((max - value) / (max - min)) * innerHeight;
    const xForSessionMinute = (minute: number) => CHART_PAD.left + ((minute - sessionOpenMinute) / visibleSessionMinutes) * innerWidth;
    const xForIndex = (index: number) => xForSessionMinute(candles[index].sessionMinute);
    const points = candles.map((item, index) => `${xForIndex(index)},${yForValue(item.close)}`).join(" ");
    const areaPoints = `${xForIndex(0)},${CHART_HEIGHT - CHART_PAD.bottom} ${points} ${xForIndex(candles.length - 1)},${CHART_HEIGHT - CHART_PAD.bottom}`;
    const gridValues = Array.from({ length: 5 }, (_, index) => min + ((max - min) * index) / 4).reverse();
    const sessionTicks = [0, 0.25, 0.5, 0.75, 1].map((position) => sessionOpenMinute + Math.round(position * visibleSessionMinutes));
    const candleWidth = Math.max(1, Math.min(12, (innerWidth / visibleSessionMinutes) * 0.72));

    return <>
      {gridValues.map((value) => (
        <g key={value}>
          <line x1={CHART_PAD.left} x2={CHART_WIDTH - CHART_PAD.right} y1={yForValue(value)} y2={yForValue(value)} className="chart-grid" />
          <text x={CHART_WIDTH - CHART_PAD.right + 10} y={yForValue(value) + 4} className="chart-axis">{value.toFixed(2)}</text>
        </g>
      ))}
      {sessionTicks.map((minute, index) => (
        <text key={minute} x={xForSessionMinute(minute)} y={CHART_HEIGHT - 14} textAnchor={index === 0 ? "start" : index === sessionTicks.length - 1 ? "end" : "middle"} className="chart-axis">
          {formatSessionMinute(minute)}
        </text>
      ))}
      {chartType === "Candles" && candles.map((item, index) => {
        const rising = item.close >= item.open;
        return (
          <g key={`${item.time}-${index}`} className={rising ? "candle-positive" : "candle-negative"}>
            <line x1={xForIndex(index)} x2={xForIndex(index)} y1={yForValue(item.high)} y2={yForValue(item.low)} />
            <rect x={xForIndex(index) - candleWidth / 2} y={yForValue(Math.max(item.open, item.close))} width={candleWidth} height={Math.max(2, Math.abs(yForValue(item.open) - yForValue(item.close)))} rx="1" />
          </g>
        );
      })}
      {chartType === "Area" && <polygon points={areaPoints} fill="url(#area-fill)" />}
      {chartType !== "Candles" && <polyline points={points} className="price-line" />}
      <line x1={CHART_PAD.left} x2={CHART_WIDTH - CHART_PAD.right} y1={yForValue(price)} y2={yForValue(price)} className="last-price-line" />
      <rect x={CHART_WIDTH - CHART_PAD.right} y={yForValue(price) - 11} width="58" height="22" rx="4" className="last-price-label" />
      <text x={CHART_WIDTH - 10} y={yForValue(price) + 4} textAnchor="end" className="last-price-text">{price.toFixed(2)}</text>
    </>;
  }, [candles, chartType, innerHeight, innerWidth, max, min, price, sessionOpenMinute, visibleSessionMinutes]);

  useEffect(() => {
    const chartShell = chartShellRef.current;
    if (!chartShell) return;
    function zoomChart(event: WheelEvent) {
      event.preventDefault();
      event.stopPropagation();
      const factor = Math.exp(-event.deltaY * 0.0018);
      setZoom((current) => Math.min(maxZoom, Math.max(1, current * factor)));
      setCursor(null);
    }
    chartShell.addEventListener("wheel", zoomChart, { passive: false });
    return () => chartShell.removeEventListener("wheel", zoomChart);
  }, [maxZoom]);

  useEffect(() => () => {
    if (pointerFrameRef.current !== null) cancelAnimationFrame(pointerFrameRef.current);
  }, []);

  function cursorFromPoint(clientX: number, clientY: number, locked: boolean, target: SVGSVGElement) {
    const screenTransform = target.getScreenCTM();
    if (!screenTransform) return;
    const svgPoint = new DOMPoint(clientX, clientY).matrixTransform(screenTransform.inverse());
    const svgX = svgPoint.x;
    const svgY = svgPoint.y;
    const cursorX = Math.min(CHART_WIDTH - CHART_PAD.right, Math.max(CHART_PAD.left, svgX));
    const cursorY = Math.min(CHART_HEIGHT - CHART_PAD.bottom, Math.max(CHART_PAD.top, svgY));
    const minute = Math.min(sessionCloseMinute, Math.max(
      sessionOpenMinute,
      Math.round(sessionOpenMinute + ((cursorX - CHART_PAD.left) / innerWidth) * visibleSessionMinutes),
    ));
    const cursorPrice = max - ((cursorY - CHART_PAD.top) / innerHeight) * (max - min);
    setCursor({ x: cursorX, y: cursorY, minute, time: formatSessionMinute(minute), price: cursorPrice, locked });
  }

  function scheduleCursorUpdate(clientX: number, clientY: number, target: SVGSVGElement) {
    pendingPointerRef.current = { clientX, clientY, target };
    if (pointerFrameRef.current !== null) return;
    pointerFrameRef.current = requestAnimationFrame(() => {
      pointerFrameRef.current = null;
      const pending = pendingPointerRef.current;
      if (pending) cursorFromPoint(pending.clientX, pending.clientY, false, pending.target);
    });
  }

  function handleChartKeyDown(event: React.KeyboardEvent<SVGSVGElement>) {
    if (event.key === "Escape") {
      setCursor(null);
      return;
    }
    if (["+", "="].includes(event.key)) {
      event.preventDefault();
      setZoom((current) => Math.min(maxZoom, current * 1.18));
      return;
    }
    if (["-", "_"].includes(event.key)) {
      event.preventDefault();
      setZoom((current) => Math.max(1, current / 1.18));
      return;
    }
    if (event.key === "0") {
      event.preventDefault();
      setZoom(1);
      return;
    }
    if (!["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Enter", " "].includes(event.key)) return;
    event.preventDefault();
    const currentMinute = cursor?.minute ?? candles.at(-1)!.sessionMinute;
    const nextMinute = event.key === "ArrowLeft"
      ? Math.max(sessionOpenMinute, currentMinute - 1)
      : event.key === "ArrowRight"
        ? Math.min(sessionCloseMinute, currentMinute + 1)
        : currentMinute;
    const currentPrice = cursor?.price ?? last.close;
    const priceStep = (max - min) / 100;
    const nextPrice = event.key === "ArrowUp"
      ? Math.min(max, currentPrice + priceStep)
      : event.key === "ArrowDown"
        ? Math.max(min, currentPrice - priceStep)
        : currentPrice;
    setCursor({ x: xForMinute(nextMinute), y: y(nextPrice), minute: nextMinute, time: formatSessionMinute(nextMinute), price: nextPrice, locked: true });
  }

  return (
    <div ref={chartShellRef} className="chart-shell" aria-label={`${symbol} price chart. Last price ${money.format(price)}. ${paused ? "Live updates paused." : "Live updates active."}`}>
      <div className="chart-context">
        <span>{hasMarketData ? "Market data" : "Simulated sample data"} · Regular session · Times shown in UTC</span>
        <div className="chart-zoom-meta">
          <span className="chart-zoom-hint">Scroll to zoom · {Math.round(zoom * 100)}%</span>
          <button type="button" onClick={() => { setZoom(1); setCursor(null); }} disabled={zoom <= 1}>Reset</button>
        </div>
      </div>
      <p className="sr-only">{symbol} intraday {chartType.toLowerCase()} chart with {candles.length} OHLC bars. Session low {money.format(min + 0.8)}, session high {money.format(max - 0.8)}. Use arrow keys to inspect time and price coordinates.</p>
      <svg
        ref={chartRef}
        className="price-chart"
        viewBox={`0 0 ${CHART_WIDTH} ${CHART_HEIGHT}`}
        role="img"
        tabIndex={0}
        aria-label={`${symbol} intraday ${chartType.toLowerCase()} price chart. Scroll to zoom, double-click to reset, or use plus, minus, and zero keys.`}
        aria-describedby="chart-cursor-readout"
        onPointerMove={(event) => {
          if (!cursor?.locked) scheduleCursorUpdate(event.clientX, event.clientY, event.currentTarget);
        }}
        onPointerDown={(event) => {
          event.currentTarget.focus();
          cursorFromPoint(event.clientX, event.clientY, true, event.currentTarget);
        }}
        onPointerLeave={() => setCursor((current) => current?.locked ? current : null)}
        onDoubleClick={() => {
          setZoom(1);
          setCursor(null);
        }}
        onKeyDown={handleChartKeyDown}
      >
        <defs>
          <linearGradient id="area-fill" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stopColor="#38bdf8" stopOpacity="0.3" />
            <stop offset="100%" stopColor="#38bdf8" stopOpacity="0" />
          </linearGradient>
        </defs>
        {chartPlot}
        {cursor && <g className="chart-crosshair" aria-hidden="true">
          <line x1={cursor.x} x2={cursor.x} y1={CHART_PAD.top} y2={CHART_HEIGHT - CHART_PAD.bottom} />
          <line x1={CHART_PAD.left} x2={CHART_WIDTH - CHART_PAD.right} y1={cursor.y} y2={cursor.y} />
          <circle cx={cursor.x} cy={cursor.y} r="4" />
          <rect x={cursorPriceLabelX} y={cursor.y - 11} width={cursorPriceLabelWidth} height="22" rx="4" />
          <text x={cursorPriceLabelX + cursorPriceLabelWidth - 5} y={cursor.y + 4} textAnchor="end">{cursor.price.toFixed(2)}</text>
          <rect x={Math.min(CHART_WIDTH - CHART_PAD.right - 70, Math.max(CHART_PAD.left, cursor.x - 35))} y={CHART_HEIGHT - CHART_PAD.bottom + 7} width="70" height="22" rx="4" />
          <text x={Math.min(CHART_WIDTH - CHART_PAD.right - 35, Math.max(CHART_PAD.left + 35, cursor.x))} y={CHART_HEIGHT - CHART_PAD.bottom + 22} textAnchor="middle">{cursor.time} UTC</text>
        </g>}
      </svg>
      <output id="chart-cursor-readout" className="sr-only" aria-live="polite">
        {cursor ? `${cursor.time} Coordinated Universal Time, price ${money.format(cursor.price)}${cursor.locked ? ", crosshair pinned" : ""}.` : "No chart coordinate selected."}
      </output>
      <details className="chart-data-table">
        <summary>View latest OHLC data ({hasMarketData ? "UTC" : "simulated UTC sample"})</summary>
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
  const [activeView, setActiveView] = useState<WorkspaceView>("graph");
  const [workspaceMenuOpen, setWorkspaceMenuOpen] = useState(false);
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
  const [marketClock, setMarketClock] = useState<MarketClockState>({
    status: "LOADING",
    timestamp: null,
    nextOpen: null,
    nextClose: null,
  });
  const [, setChartRevision] = useState(0);
  const [instrumentQuery, setInstrumentQuery] = useState("AAPL · Apple Inc.");
  const [searchResults, setSearchResults] = useState<CatalogueInstrument[]>([]);
  const [searchLoading, setSearchLoading] = useState(false);
  const [searchOpen, setSearchOpen] = useState(false);
  const searchInputRef = useRef<HTMLInputElement>(null);
  const workspaceMenuRef = useRef<HTMLDivElement>(null);
  const socketRef = useRef<WebSocket | null>(null);
  const marketDataStatusRef = useRef<MarketDataStatus>("CONNECTING");
  const subscribedSymbolRef = useRef(symbol);
  const activeSymbolRef = useRef(symbol);
  const pausedRef = useRef(paused);
  const displayedMarketDataStatus = paused ? "Quote updates paused" : marketData.message;
  const marketDataTone = marketData.status.toLowerCase();
  const marketSessionTone = marketClock.status.toLowerCase().replace("_", "-");
  const marketSessionLabel = {
    LOADING: "Checking market",
    OPEN: "Market open",
    PRE_OPEN: "Pre-open",
    CLOSED: "Market closed",
    UNAVAILABLE: "Session unavailable",
  }[marketClock.status];
  const marketSessionDetail = marketClock.status === "OPEN"
    ? `Closes ${utcTimeLabel(marketClock.nextClose)}`
    : marketClock.status === "PRE_OPEN"
      ? `Opens ${utcTimeLabel(marketClock.nextOpen)}`
      : marketClock.status === "CLOSED"
        ? `Next open ${utcTimeLabel(marketClock.nextOpen)}`
        : "US equity session time in UTC";

  const quote = instruments[symbol];
  const orderBookLevels = useMemo(() => ({
    asks: Array.from({ length: 6 }, (_, index) => ({
      price: quote.ask + index * 0.02,
      size: 90 + ((index + 3) * 137) % 720,
    })).reverse(),
    bids: Array.from({ length: 6 }, (_, index) => ({
      price: quote.bid - index * 0.02,
      size: 110 + ((index + 5) * 113) % 680,
    })),
  }), [quote.ask, quote.bid]);
  const filledOrders = useMemo(() => orders.filter((order) => order.status === "FILLED"), [orders]);
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
    function closeWorkspaceMenu(event: MouseEvent) {
      if (!workspaceMenuRef.current?.contains(event.target as Node)) setWorkspaceMenuOpen(false);
    }
    function closeWorkspaceMenuWithEscape(event: KeyboardEvent) {
      if (event.key === "Escape") setWorkspaceMenuOpen(false);
    }
    document.addEventListener("pointerdown", closeWorkspaceMenu);
    document.addEventListener("keydown", closeWorkspaceMenuWithEscape);
    return () => {
      document.removeEventListener("pointerdown", closeWorkspaceMenu);
      document.removeEventListener("keydown", closeWorkspaceMenuWithEscape);
    };
  }, []);

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
          time: utcTimeLabel(item.created_at),
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
    const controller = new AbortController();
    async function loadMarketClock() {
      try {
        const response = await fetch(`${API_URL}/api/v1/market/clock`, { signal: controller.signal });
        const result = await response.json() as {
          status?: "OPEN" | "PRE_OPEN" | "CLOSED";
          timestamp?: string;
          nextOpen?: string;
          nextClose?: string;
        };
        if (!response.ok || !result.status) throw new Error("market_clock_unavailable");
        setMarketClock({
          status: result.status,
          timestamp: result.timestamp ?? null,
          nextOpen: result.nextOpen ?? null,
          nextClose: result.nextClose ?? null,
        });
      } catch (error) {
        if (error instanceof DOMException && error.name === "AbortError") return;
        setMarketClock((current) => ({ ...current, status: "UNAVAILABLE" }));
      }
    }
    void loadMarketClock();
    const intervalId = window.setInterval(() => void loadMarketClock(), 60_000);
    return () => {
      controller.abort();
      window.clearInterval(intervalId);
    };
  }, []);

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
      socket.addEventListener("open", () => {
        socket.send(JSON.stringify({ action: "authenticate", accessToken }));
      });
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
          open?: number;
          high?: number;
          low?: number;
          close?: number;
          volume?: number;
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
          if (mergeHistoricalBars(event.symbol, event.bars)) {
            setChartRevision((current) => current + 1);
          }
          return;
        }
        if (event.symbol !== activeSymbolRef.current) return;
        if ((event.type === "bar" || event.type === "updatedBar") && mergeLiveBar(event.symbol, event) && !pausedRef.current) {
          setChartRevision((current) => current + 1);
        }
        if (pausedRef.current) return;
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
  }, [accessToken]);

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
        time: result.created_at ? utcTimeLabel(result.created_at) : utcTimeLabel(new Date().toISOString()),
      };
      setOrders((current) => [newOrder, ...current]);
      setActiveView(newOrder.status === "FILLED" ? "fills" : "audit");

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
        <div className="topbar-actions">
          <div className="workspace-menu" ref={workspaceMenuRef}>
            <button className="workspace-menu-trigger" type="button" aria-haspopup="menu" aria-expanded={workspaceMenuOpen} onClick={() => setWorkspaceMenuOpen((current) => !current)}>
              <Menu aria-hidden="true" />
              <span>{workspaceMenuItems.find((item) => item.id === activeView)?.label}</span>
              <ChevronDown aria-hidden="true" />
            </button>
            {workspaceMenuOpen && <div className="workspace-menu-popover" role="menu" aria-label="Choose workspace feature">
              <div className="workspace-menu-title"><SlidersHorizontal aria-hidden="true" /><span><strong>Workspace view</strong><small>Choose one feature to focus on</small></span></div>
              {workspaceMenuItems.map((item) => {
                const Icon = item.icon;
                return <button key={item.id} type="button" role="menuitemradio" aria-checked={activeView === item.id} className={activeView === item.id ? "active" : ""} onClick={() => { setActiveView(item.id); setWorkspaceMenuOpen(false); }}><Icon aria-hidden="true" /><span><strong>{item.label}</strong><small>{item.description}</small></span></button>;
              })}
            </div>}
          </div>
          <span className={`market-status ${marketDataTone}`} role="status" aria-atomic="true">
            <i aria-hidden="true" />
            <b>{marketData.status}{marketData.status === "QUEUED" && marketData.queuePosition ? ` · #${marketData.queuePosition}` : ""}</b>
          </span>
          <ThemeToggle />
          <button className="icon-button notification-button" aria-label="Notifications"><Bell aria-hidden="true" /><span className="notification-dot" /></button>
          <button className="account-button" onClick={onSignOut} aria-label={`Sign out ${userName}`} title="Sign out"><span>{userName.split(/\s+/).map((part) => part[0]).join("").slice(0, 2).toUpperCase() || "ST"}</span><div><strong>{userName}</strong><small>Demo account · Sign out</small></div><ChevronDown aria-hidden="true" /></button>
        </div>
      </header>

      <div className="workspace-grid">
        <aside className={`watchlist-panel ${mobileWatchlist ? "mobile-open" : ""}`}>
          <div className="panel-heading"><div><small>MARKET</small><h2>Watchlist</h2></div><button className="icon-button mobile-only" aria-label="Close watchlist" onClick={() => setMobileWatchlist(false)}><X aria-hidden="true" /></button></div>
          <label className="instrument-search watchlist-search" onBlur={(event) => {
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
              onFocus={(event) => { setSearchOpen(true); event.currentTarget.select(); }}
              onChange={(event) => { setInstrumentQuery(event.target.value); setSearchResults([]); setSearchLoading(true); setSearchOpen(true); }}
              onKeyDown={(event) => { if (event.key === "Escape") setSearchOpen(false); }}
              placeholder="Search symbol or company"
            />
            {searchOpen && instrumentQuery.trim() && instrumentQuery.trim() !== `${symbol} · ${quote.name}` && <div id="instrument-results" className="instrument-results" role="listbox" aria-label="Instrument search results">
              {searchLoading && <p role="status">Searching instruments…</p>}
              {!searchLoading && searchResults.map((instrument) => <button key={instrument.symbol} type="button" role="option" aria-selected="false" onClick={() => selectCatalogueInstrument(instrument)}>
                <span><strong>{instrument.symbol}</strong><small>{instrument.exchange}</small></span>
                <span>{instrument.name}</span>
              </button>)}
              {!searchLoading && searchResults.length === 0 && <p>No matching instruments. Try a symbol or company name.</p>}
            </div>}
          </label>
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
          <div className={`data-source ${marketDataTone}`}><ShieldCheck aria-hidden="true" /><span><strong>Alpaca market data · {marketData.status}</strong><small>{displayedMarketDataStatus}</small><small>{lastUpdateLabel(marketData.lastUpdatedAt)}</small></span></div>
        </aside>

        <section className="main-workspace">
          <div className="quote-strip">
            <div className="quote-identity"><span className="instrument-avatar">{symbol.slice(0, 1)}</span><div><div><h1>{symbol}</h1><span>{quote.exchange || "US"}</span><span className={`instrument-session ${marketSessionTone}`} role="status" aria-atomic="true"><i aria-hidden="true" />{marketSessionLabel}</span></div><p>{quote.name} · USD · {marketSessionDetail}</p></div></div>
            <div className="quote-price"><strong>{quote.price.toFixed(2)}</strong><span className={quote.change >= 0 ? "positive" : "negative"}>{quote.change >= 0 ? "+" : ""}{(quote.price * quote.change / 100).toFixed(2)} ({quote.change >= 0 ? "+" : ""}{quote.change.toFixed(2)}%)</span></div>
            <dl className="quote-stats"><div><dt>Bid</dt><dd>{quote.bid.toFixed(2)}</dd></div><div><dt>Ask</dt><dd>{quote.ask.toFixed(2)}</dd></div><div><dt>Spread</dt><dd>{(quote.ask - quote.bid).toFixed(2)}</dd></div><div><dt>Volume</dt><dd>42.8M</dd></div></dl>
          </div>

          {activeView === "graph" && <div className="chart-panel feature-view">
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
          </div>}

          {activeView === "orderbook" && <section className="feature-panel feature-view" aria-labelledby="orderbook-title">
            <div className="feature-panel-heading"><div><span>MARKET DEPTH</span><h2 id="orderbook-title">{symbol} order book</h2><p>Indicative simulated depth around the current bid and ask.</p></div><span className="spread-chip">Spread {money.format(quote.ask - quote.bid)}</span></div>
            <div className="orderbook-grid">
              <div className="orderbook-side asks"><div className="depth-heading"><strong>Ask orders</strong><span>Price / Size</span></div>{orderBookLevels.asks.map((level) => <div key={level.price} className="depth-row"><span className="depth-bar" style={{ width: `${Math.min(100, level.size / 8)}%` }} /><strong>{level.price.toFixed(2)}</strong><span>{number.format(level.size)}</span></div>)}</div>
              <div className="orderbook-mid"><span>Mid price</span><strong>{((quote.bid + quote.ask) / 2).toFixed(2)}</strong><small>{quote.bid.toFixed(2)} bid · {quote.ask.toFixed(2)} ask</small></div>
              <div className="orderbook-side bids"><div className="depth-heading"><strong>Bid orders</strong><span>Price / Size</span></div>{orderBookLevels.bids.map((level) => <div key={level.price} className="depth-row"><span className="depth-bar" style={{ width: `${Math.min(100, level.size / 8)}%` }} /><strong>{level.price.toFixed(2)}</strong><span>{number.format(level.size)}</span></div>)}</div>
            </div>
          </section>}

          {activeView === "fills" && <section className="feature-panel feature-view" aria-labelledby="fills-title">
            <div className="feature-panel-heading"><div><span>EXECUTIONS</span><h2 id="fills-title">Fill book</h2><p>Completed simulated trades for this account.</p></div><span className="count-chip">{filledOrders.length} fills</span></div>
            <div className="table-scroll"><table><thead><tr><th>Fill</th><th>Time</th><th>Symbol</th><th>Side</th><th>Type</th><th>Qty</th><th>Fill price</th><th>Value</th></tr></thead><tbody>
              {filledOrders.map((order) => <tr key={order.id}><td><strong>{order.id.slice(0, 8).toUpperCase()}</strong></td><td>{order.time}</td><td>{order.symbol}</td><td className={order.side === "BUY" ? "positive" : "negative"}>{order.side}</td><td>{order.type}</td><td>{order.quantity}</td><td>{money.format(order.price)}</td><td>{money.format(order.price * order.quantity)}</td></tr>)}
              {filledOrders.length === 0 && <tr><td className="empty-table" colSpan={8}>No fills yet. Completed orders will appear here.</td></tr>}
            </tbody></table></div>
          </section>}

          {activeView === "positions" && <section className="feature-panel feature-view" aria-labelledby="positions-title">
            <div className="feature-panel-heading"><div><span>PORTFOLIO</span><h2 id="positions-title">Open positions</h2><p>Current holdings, market value, and unrealized profit or loss.</p></div><span className="count-chip">{positions.length} positions</span></div>
            <div className="table-scroll"><table><thead><tr><th>Symbol</th><th>Qty</th><th>Avg price</th><th>Last</th><th>Market value</th><th>Unrealized P&amp;L</th></tr></thead><tbody>{positions.map((position) => {
              const positionInstrument = instruments[position.symbol] ?? { name: position.symbol, exchange: "US", price: position.averagePrice, change: 0, bid: position.averagePrice, ask: position.averagePrice };
              const last = positionInstrument.price;
              const pnl = (last - position.averagePrice) * position.quantity;
              return <tr key={position.symbol}><td><strong>{position.symbol}</strong><small>{positionInstrument.name}</small></td><td>{position.quantity}</td><td>{money.format(position.averagePrice)}</td><td>{money.format(last)}</td><td>{money.format(last * position.quantity)}</td><td className={pnl >= 0 ? "positive" : "negative"}>{pnl >= 0 ? "+" : ""}{money.format(pnl)}</td></tr>;
            })}{positions.length === 0 && <tr><td className="empty-table" colSpan={6}>No open positions. Filled orders will update this view.</td></tr>}</tbody></table></div>
          </section>}

          {activeView === "audit" && <section className="feature-panel feature-view" aria-labelledby="audit-title">
            <div className="feature-panel-heading"><div><span>ACCOUNT HISTORY</span><h2 id="audit-title">Audit trail</h2><p>A chronological record of simulated order activity and pending orders.</p></div><span className="count-chip">{orders.length} events</span></div>
            <div className="audit-list">{orders.map((order) => <article key={order.id}><span className={`audit-marker ${order.status.toLowerCase()}`} aria-hidden="true" /><div><strong>{order.side} {order.quantity} {order.symbol}</strong><p>{order.type} order {order.status.toLowerCase()} at {money.format(order.price)}.</p></div><span><time>{order.time}</time><small>{order.id.slice(0, 8).toUpperCase()}</small>{order.status === "ACCEPTED" && <button className="table-action" type="button" onClick={() => void cancelOrder(order.id)}>Cancel order</button>}</span></article>)}{orders.length === 0 && <div className="empty-feature"><ClipboardList aria-hidden="true" /><strong>No audit events yet</strong><p>Order submissions, fills, and cancellations will be recorded here.</p></div>}</div>
          </section>}
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
