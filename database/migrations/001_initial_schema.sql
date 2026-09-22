CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TABLE users (
    user_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name VARCHAR(160) NOT NULL,
    email VARCHAR(320) NOT NULL,
    email_verified_at TIMESTAMPTZ,
    status VARCHAR(24) NOT NULL DEFAULT 'ACTIVE' CHECK (status IN ('PENDING_VERIFICATION', 'ACTIVE', 'SUSPENDED', 'CLOSED')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE UNIQUE INDEX users_email_unique_ci ON users (lower(email));

CREATE TABLE user_credentials (
    user_id UUID PRIMARY KEY REFERENCES users(user_id) ON DELETE CASCADE,
    password_hash TEXT NOT NULL,
    password_algorithm VARCHAR(32) NOT NULL DEFAULT 'argon2id',
    password_changed_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE otp_challenges (
    challenge_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    purpose VARCHAR(32) NOT NULL CHECK (purpose IN ('VERIFY_EMAIL', 'RESET_PASSWORD')),
    otp_hash TEXT NOT NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0 CHECK (attempt_count >= 0),
    expires_at TIMESTAMPTZ NOT NULL,
    consumed_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX otp_challenges_user_active_idx ON otp_challenges (user_id, purpose, expires_at DESC) WHERE consumed_at IS NULL;

CREATE TABLE refresh_tokens (
    token_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    family_id UUID NOT NULL,
    token_hash TEXT NOT NULL UNIQUE,
    expires_at TIMESTAMPTZ NOT NULL,
    revoked_at TIMESTAMPTZ,
    replaced_by_token_id UUID REFERENCES refresh_tokens(token_id),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX refresh_tokens_user_active_idx ON refresh_tokens (user_id, expires_at DESC) WHERE revoked_at IS NULL;

CREATE TABLE trading_accounts (
    account_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES users(user_id),
    account_name VARCHAR(120) NOT NULL DEFAULT 'Primary simulation',
    base_currency CHAR(3) NOT NULL DEFAULT 'USD',
    cash NUMERIC(24, 8) NOT NULL DEFAULT 100000 CHECK (cash >= 0),
    reserved_cash NUMERIC(24, 8) NOT NULL DEFAULT 0 CHECK (reserved_cash >= 0),
    status VARCHAR(24) NOT NULL DEFAULT 'ACTIVE' CHECK (status IN ('ACTIVE', 'RESTRICTED', 'CLOSED')),
    version BIGINT NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX trading_accounts_user_idx ON trading_accounts (user_id, status);

CREATE TABLE instruments (
    instrument_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    symbol VARCHAR(64) NOT NULL,
    name VARCHAR(240) NOT NULL,
    asset_class VARCHAR(32) NOT NULL CHECK (asset_class IN ('EQUITY', 'ETF', 'FUTURE', 'OPTION', 'FOREX', 'CRYPTO')),
    exchange VARCHAR(64) NOT NULL,
    currency CHAR(3) NOT NULL,
    tick_size NUMERIC(24, 10) NOT NULL CHECK (tick_size > 0),
    lot_size NUMERIC(24, 10) NOT NULL DEFAULT 1 CHECK (lot_size > 0),
    contract_size NUMERIC(24, 10) NOT NULL DEFAULT 1 CHECK (contract_size > 0),
    expiry TIMESTAMPTZ,
    market_data_provider VARCHAR(64) NOT NULL,
    provider_symbol VARCHAR(128) NOT NULL,
    status VARCHAR(24) NOT NULL DEFAULT 'ACTIVE' CHECK (status IN ('ACTIVE', 'HALTED', 'INACTIVE', 'EXPIRED')),
    metadata JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (exchange, symbol, expiry),
    UNIQUE (market_data_provider, provider_symbol)
);
CREATE INDEX instruments_symbol_idx ON instruments (symbol);
CREATE INDEX instruments_asset_status_idx ON instruments (asset_class, status);

CREATE TABLE strategy_orders (
    strategy_order_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES users(user_id),
    account_id UUID NOT NULL REFERENCES trading_accounts(account_id),
    strategy_type VARCHAR(48) NOT NULL,
    status VARCHAR(32) NOT NULL DEFAULT 'NEW' CHECK (status IN ('NEW', 'ACCEPTED', 'PARTIALLY_FILLED', 'FILLED', 'CANCELLED', 'REJECTED')),
    client_order_id VARCHAR(128) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (user_id, client_order_id)
);
CREATE INDEX strategy_orders_user_status_idx ON strategy_orders (user_id, status, created_at DESC);

CREATE TABLE strategy_legs (
    strategy_leg_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    strategy_order_id UUID NOT NULL REFERENCES strategy_orders(strategy_order_id),
    leg_number SMALLINT NOT NULL CHECK (leg_number > 0),
    instrument_id UUID NOT NULL REFERENCES instruments(instrument_id),
    side VARCHAR(4) NOT NULL CHECK (side IN ('BUY', 'SELL')),
    quantity NUMERIC(24, 8) NOT NULL CHECK (quantity > 0),
    ratio NUMERIC(16, 8) NOT NULL DEFAULT 1 CHECK (ratio > 0),
    price NUMERIC(24, 8) CHECK (price > 0),
    UNIQUE (strategy_order_id, leg_number)
);

CREATE TABLE orders (
    order_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES users(user_id),
    account_id UUID NOT NULL REFERENCES trading_accounts(account_id),
    instrument_id UUID NOT NULL REFERENCES instruments(instrument_id),
    strategy_order_id UUID REFERENCES strategy_orders(strategy_order_id),
    client_order_id VARCHAR(128) NOT NULL,
    side VARCHAR(4) NOT NULL CHECK (side IN ('BUY', 'SELL')),
    order_type VARCHAR(24) NOT NULL CHECK (order_type IN ('MARKET', 'LIMIT', 'STOP', 'STOP_LIMIT', 'ICEBERG')),
    quantity NUMERIC(24, 8) NOT NULL CHECK (quantity > 0),
    remaining_quantity NUMERIC(24, 8) NOT NULL CHECK (remaining_quantity >= 0),
    limit_price NUMERIC(24, 8) CHECK (limit_price > 0),
    stop_price NUMERIC(24, 8) CHECK (stop_price > 0),
    total_quantity NUMERIC(24, 8) CHECK (total_quantity > 0),
    display_quantity NUMERIC(24, 8) CHECK (display_quantity > 0),
    visible_remaining_quantity NUMERIC(24, 8) CHECK (visible_remaining_quantity >= 0),
    status VARCHAR(32) NOT NULL DEFAULT 'NEW' CHECK (status IN ('NEW', 'ACCEPTED', 'PARTIALLY_FILLED', 'FILLED', 'CANCELLED', 'REJECTED')),
    rejection_reason TEXT,
    average_fill_price NUMERIC(24, 8) CHECK (average_fill_price > 0),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    accepted_at TIMESTAMPTZ,
    filled_at TIMESTAMPTZ,
    cancelled_at TIMESTAMPTZ,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    version BIGINT NOT NULL DEFAULT 0,
    UNIQUE (user_id, client_order_id),
    CHECK (remaining_quantity <= quantity),
    CHECK ((order_type NOT IN ('LIMIT', 'STOP_LIMIT')) OR limit_price IS NOT NULL),
    CHECK ((order_type NOT IN ('STOP', 'STOP_LIMIT')) OR stop_price IS NOT NULL),
    CHECK ((order_type <> 'ICEBERG') OR (total_quantity IS NOT NULL AND display_quantity IS NOT NULL AND display_quantity <= total_quantity))
);
CREATE INDEX orders_user_created_idx ON orders (user_id, created_at DESC);
CREATE INDEX orders_account_status_idx ON orders (account_id, status, created_at DESC);
CREATE INDEX orders_instrument_status_idx ON orders (instrument_id, status);

CREATE TABLE trades (
    trade_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    order_id UUID NOT NULL REFERENCES orders(order_id),
    user_id UUID NOT NULL REFERENCES users(user_id),
    account_id UUID NOT NULL REFERENCES trading_accounts(account_id),
    instrument_id UUID NOT NULL REFERENCES instruments(instrument_id),
    side VARCHAR(4) NOT NULL CHECK (side IN ('BUY', 'SELL')),
    quantity NUMERIC(24, 8) NOT NULL CHECK (quantity > 0),
    price NUMERIC(24, 8) NOT NULL CHECK (price > 0),
    fee NUMERIC(24, 8) NOT NULL DEFAULT 0 CHECK (fee >= 0),
    liquidity_flag VARCHAR(16),
    executed_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX trades_user_executed_idx ON trades (user_id, executed_at DESC);
CREATE INDEX trades_account_instrument_idx ON trades (account_id, instrument_id, executed_at DESC);

CREATE TABLE positions (
    position_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id UUID NOT NULL REFERENCES trading_accounts(account_id),
    user_id UUID NOT NULL REFERENCES users(user_id),
    instrument_id UUID NOT NULL REFERENCES instruments(instrument_id),
    quantity NUMERIC(24, 8) NOT NULL DEFAULT 0,
    average_price NUMERIC(24, 8) NOT NULL DEFAULT 0 CHECK (average_price >= 0),
    realized_pnl NUMERIC(24, 8) NOT NULL DEFAULT 0,
    version BIGINT NOT NULL DEFAULT 0,
    opened_at TIMESTAMPTZ,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (account_id, instrument_id)
);
CREATE INDEX positions_user_idx ON positions (user_id, account_id);

CREATE TABLE portfolio_snapshots (
    snapshot_id BIGSERIAL PRIMARY KEY,
    account_id UUID NOT NULL REFERENCES trading_accounts(account_id),
    cash NUMERIC(24, 8) NOT NULL,
    buying_power NUMERIC(24, 8) NOT NULL,
    portfolio_value NUMERIC(24, 8) NOT NULL,
    realized_pnl NUMERIC(24, 8) NOT NULL,
    unrealized_pnl NUMERIC(24, 8) NOT NULL,
    captured_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX portfolio_snapshots_account_time_idx ON portfolio_snapshots (account_id, captured_at DESC);

CREATE TABLE watchlists (
    watchlist_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    name VARCHAR(120) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (user_id, name)
);

CREATE TABLE watchlist_items (
    watchlist_id UUID NOT NULL REFERENCES watchlists(watchlist_id) ON DELETE CASCADE,
    instrument_id UUID NOT NULL REFERENCES instruments(instrument_id),
    sort_order INTEGER NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (watchlist_id, instrument_id)
);

CREATE TABLE risk_limits (
    risk_limit_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID REFERENCES users(user_id),
    account_id UUID REFERENCES trading_accounts(account_id),
    max_order_quantity NUMERIC(24, 8) NOT NULL,
    max_position_value NUMERIC(24, 8) NOT NULL,
    max_position_percent NUMERIC(7, 4) NOT NULL CHECK (max_position_percent > 0 AND max_position_percent <= 1),
    max_daily_loss NUMERIC(24, 8) NOT NULL,
    max_order_rate_per_minute INTEGER NOT NULL CHECK (max_order_rate_per_minute > 0),
    restricted_instrument_ids UUID[] NOT NULL DEFAULT '{}',
    settings JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (user_id IS NOT NULL OR account_id IS NOT NULL)
);
CREATE INDEX risk_limits_user_account_idx ON risk_limits (user_id, account_id);

CREATE TABLE compliance_alerts (
    alert_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES users(user_id),
    account_id UUID REFERENCES trading_accounts(account_id),
    order_id UUID REFERENCES orders(order_id),
    rule VARCHAR(96) NOT NULL,
    severity VARCHAR(16) NOT NULL CHECK (severity IN ('LOW', 'MEDIUM', 'HIGH', 'CRITICAL')),
    description TEXT NOT NULL,
    evidence JSONB NOT NULL DEFAULT '{}'::jsonb,
    status VARCHAR(24) NOT NULL DEFAULT 'OPEN' CHECK (status IN ('OPEN', 'IN_REVIEW', 'DISMISSED', 'RESOLVED')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    reviewed_at TIMESTAMPTZ,
    reviewed_by UUID REFERENCES users(user_id)
);
CREATE INDEX compliance_alerts_user_status_idx ON compliance_alerts (user_id, status, created_at DESC);
CREATE INDEX compliance_alerts_severity_status_idx ON compliance_alerts (severity, status, created_at DESC);

INSERT INTO instruments (symbol, name, asset_class, exchange, currency, tick_size, lot_size, contract_size, market_data_provider, provider_symbol)
VALUES
  ('AAPL', 'Apple Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'AAPL'),
  ('NVDA', 'NVIDIA Corporation', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'NVDA'),
  ('MSFT', 'Microsoft Corporation', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'MSFT'),
  ('GOOGL', 'Alphabet Inc. Class A', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'GOOGL'),
  ('AMZN', 'Amazon.com Inc.', 'EQUITY', 'NASDAQ', 'USD', 0.01, 1, 1, 'ALPACA', 'AMZN');
