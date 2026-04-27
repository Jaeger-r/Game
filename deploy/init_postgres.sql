CREATE TABLE IF NOT EXISTS user_account (
    u_id SERIAL PRIMARY KEY,
    u_name VARCHAR(64) NOT NULL UNIQUE,
    u_password VARCHAR(96) NOT NULL,
    u_tel BIGINT NOT NULL UNIQUE
);

CREATE TABLE IF NOT EXISTS user_basic_information (
    u_id INT PRIMARY KEY REFERENCES user_account(u_id) ON DELETE CASCADE,
    u_name VARCHAR(64) NOT NULL,
    u_health INT NOT NULL DEFAULT 100,
    u_mana INT NOT NULL DEFAULT 0,
    u_attackpower INT NOT NULL DEFAULT 0,
    u_magicattack INT NOT NULL DEFAULT 0,
    u_independentattack INT NOT NULL DEFAULT 0,
    u_attackrange INT NOT NULL DEFAULT 0,
    u_experience BIGINT NOT NULL DEFAULT 0,
    u_level INT NOT NULL DEFAULT 1,
    u_defence INT NOT NULL DEFAULT 0,
    u_magicdefence INT NOT NULL DEFAULT 0,
    u_strength INT NOT NULL DEFAULT 0,
    u_intelligence INT NOT NULL DEFAULT 0,
    u_vitality INT NOT NULL DEFAULT 0,
    u_spirit INT NOT NULL DEFAULT 0,
    u_critrate REAL NOT NULL DEFAULT 0,
    u_magiccritrate REAL NOT NULL DEFAULT 0,
    u_critdamage REAL NOT NULL DEFAULT 0,
    u_attackspeed REAL NOT NULL DEFAULT 0,
    u_movespeed REAL NOT NULL DEFAULT 0,
    u_castspeed REAL NOT NULL DEFAULT 0,
    u_position_x REAL NOT NULL DEFAULT 100,
    u_position_y REAL NOT NULL DEFAULT 100,
    u_equipment_weapon INT NOT NULL DEFAULT 0,
    u_equipment_head INT NOT NULL DEFAULT 0,
    u_equipment_body INT NOT NULL DEFAULT 0,
    u_equipment_legs INT NOT NULL DEFAULT 0,
    u_equipment_hands INT NOT NULL DEFAULT 0,
    u_equipment_shoes INT NOT NULL DEFAULT 0,
    u_equipment_shield INT NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS user_item (
    u_id INT NOT NULL REFERENCES user_account(u_id) ON DELETE CASCADE,
    item_id INT NOT NULL,
    item_count INT NOT NULL DEFAULT 0,
    PRIMARY KEY (u_id, item_id)
);

CREATE TABLE IF NOT EXISTS user_equipment_state (
    u_id INT NOT NULL REFERENCES user_account(u_id) ON DELETE CASCADE,
    slot_index INT NOT NULL,
    item_id INT NOT NULL DEFAULT 0,
    enhance_level INT NOT NULL DEFAULT 0,
    forge_level INT NOT NULL DEFAULT 0,
    enchant_kind INT NOT NULL DEFAULT 0,
    enchant_value INT NOT NULL DEFAULT 0,
    enhance_success_count INT NOT NULL DEFAULT 0,
    forge_success_count INT NOT NULL DEFAULT 0,
    enchant_success_count INT NOT NULL DEFAULT 0,
    PRIMARY KEY (u_id, slot_index)
);

CREATE TABLE IF NOT EXISTS user_progress_state (
    u_id INT PRIMARY KEY REFERENCES user_account(u_id) ON DELETE CASCADE,
    map_id VARCHAR(64) NOT NULL DEFAULT 'BornWorld',
    quest_step INT NOT NULL DEFAULT 0,
    warehouse_unlock_tier INT NOT NULL DEFAULT 1,
    pos_x REAL NOT NULL DEFAULT 100,
    pos_y REAL NOT NULL DEFAULT 100,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS user_warehouse_item (
    u_id INT NOT NULL REFERENCES user_account(u_id) ON DELETE CASCADE,
    item_id INT NOT NULL,
    item_count INT NOT NULL DEFAULT 0,
    PRIMARY KEY (u_id, item_id)
);

CREATE TABLE IF NOT EXISTS user_inventory_journal (
    journal_id BIGSERIAL PRIMARY KEY,
    u_id INT NOT NULL REFERENCES user_account(u_id) ON DELETE CASCADE,
    state_version INT NOT NULL DEFAULT 0,
    action_key VARCHAR(64) NOT NULL,
    payload_json TEXT NOT NULL,
    status VARCHAR(16) NOT NULL DEFAULT 'pending',
    attempt_count INT NOT NULL DEFAULT 0,
    last_error VARCHAR(255),
    applied_at TIMESTAMPTZ NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_inventory_journal_status
    ON user_inventory_journal (status, created_at);
