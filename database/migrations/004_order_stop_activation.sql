ALTER TABLE orders
    ADD COLUMN IF NOT EXISTS stop_activated BOOLEAN NOT NULL DEFAULT false;

ALTER TABLE orders DROP CONSTRAINT IF EXISTS orders_stop_activation_check;
ALTER TABLE orders
    ADD CONSTRAINT orders_stop_activation_check
    CHECK (NOT stop_activated OR order_type IN ('STOP', 'STOP_LIMIT'));
