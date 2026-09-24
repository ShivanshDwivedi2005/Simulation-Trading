ALTER TABLE orders
    ADD COLUMN IF NOT EXISTS execution_price_source VARCHAR(64),
    ADD COLUMN IF NOT EXISTS execution_price_timestamp TIMESTAMPTZ;

ALTER TABLE trades
    ADD COLUMN IF NOT EXISTS price_source VARCHAR(64),
    ADD COLUMN IF NOT EXISTS price_timestamp TIMESTAMPTZ;
