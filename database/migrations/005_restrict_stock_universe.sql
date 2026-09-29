-- Keep order matching and market-data discovery limited to the selected 30 stocks.
-- Existing instruments are retained for referential integrity but are no longer tradable.
UPDATE instruments
SET status = 'INACTIVE', updated_at = now()
WHERE status = 'ACTIVE'
  AND symbol NOT IN (
    'NVDA', 'AAPL', 'MSFT', 'TSLA', 'AMZN', 'GOOGL', 'META', 'AVGO', 'AMD', 'MU',
    'ORCL', 'PLTR', 'NFLX', 'WMT', 'COST', 'HD', 'JPM', 'BAC', 'V', 'MA',
    'LLY', 'JNJ', 'UNH', 'ABBV', 'XOM', 'CVX', 'GE', 'CAT', 'BA', 'PG'
  );

INSERT INTO instruments
    (symbol, name, asset_class, exchange, currency, tick_size, lot_size, contract_size,
     market_data_provider, provider_symbol, status, matching_group)
VALUES
    ('NVDA', 'NVIDIA Corporation', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'NVDA', 'ACTIVE', 1),
    ('AAPL', 'Apple Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'AAPL', 'ACTIVE', 1),
    ('MSFT', 'Microsoft Corporation', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'MSFT', 'ACTIVE', 1),
    ('TSLA', 'Tesla Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'TSLA', 'ACTIVE', 1),
    ('AMZN', 'Amazon.com Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'AMZN', 'ACTIVE', 1),
    ('GOOGL', 'Alphabet Inc. Class A', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'GOOGL', 'ACTIVE', 1),
    ('META', 'Meta Platforms Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'META', 'ACTIVE', 1),
    ('AVGO', 'Broadcom Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'AVGO', 'ACTIVE', 1),
    ('AMD', 'Advanced Micro Devices Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'AMD', 'ACTIVE', 1),
    ('MU', 'Micron Technology Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'MU', 'ACTIVE', 1),
    ('ORCL', 'Oracle Corporation', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'ORCL', 'ACTIVE', 1),
    ('PLTR', 'Palantir Technologies Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'PLTR', 'ACTIVE', 1),
    ('NFLX', 'Netflix Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'NFLX', 'ACTIVE', 1),
    ('WMT', 'Walmart Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'WMT', 'ACTIVE', 1),
    ('COST', 'Costco Wholesale Corporation', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'COST', 'ACTIVE', 1),
    ('HD', 'Home Depot Inc.', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'HD', 'ACTIVE', 2),
    ('JPM', 'JPMorgan Chase & Co.', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'JPM', 'ACTIVE', 2),
    ('BAC', 'Bank of America Corporation', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'BAC', 'ACTIVE', 2),
    ('V', 'Visa Inc.', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'V', 'ACTIVE', 2),
    ('MA', 'Mastercard Incorporated', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'MA', 'ACTIVE', 2),
    ('LLY', 'Eli Lilly and Company', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'LLY', 'ACTIVE', 2),
    ('JNJ', 'Johnson & Johnson', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'JNJ', 'ACTIVE', 2),
    ('UNH', 'UnitedHealth Group Incorporated', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'UNH', 'ACTIVE', 2),
    ('ABBV', 'AbbVie Inc.', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'ABBV', 'ACTIVE', 2),
    ('XOM', 'Exxon Mobil Corporation', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'XOM', 'ACTIVE', 2),
    ('CVX', 'Chevron Corporation', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'CVX', 'ACTIVE', 2),
    ('GE', 'GE Aerospace', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'GE', 'ACTIVE', 2),
    ('CAT', 'Caterpillar Inc.', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'CAT', 'ACTIVE', 2),
    ('BA', 'Boeing Company', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'BA', 'ACTIVE', 2),
    ('PG', 'Procter & Gamble Company', 'EQUITY', 'NYSE', 'USD', 0.01, 1, 1, 'ALPACA', 'PG', 'ACTIVE', 2)
ON CONFLICT (market_data_provider, provider_symbol) DO UPDATE
SET name = EXCLUDED.name,
    asset_class = EXCLUDED.asset_class,
    exchange = EXCLUDED.exchange,
    currency = EXCLUDED.currency,
    tick_size = EXCLUDED.tick_size,
    lot_size = EXCLUDED.lot_size,
    contract_size = EXCLUDED.contract_size,
    status = EXCLUDED.status,
    matching_group = EXCLUDED.matching_group,
    updated_at = now();
