-- MailServer PostgreSQL schema
-- Supports SHA-512 crypt ($6$) password hashes.

CREATE TABLE IF NOT EXISTS users (
    id            SERIAL PRIMARY KEY,
    email         VARCHAR(320) UNIQUE NOT NULL,
    password_hash VARCHAR(256)        NOT NULL,  -- SHA-512 crypt $6$ hash
    maildir       VARCHAR(512)        NOT NULL DEFAULT '',  -- relative maildir path
    active        BOOLEAN             NOT NULL DEFAULT TRUE,
    quota_bytes   BIGINT              DEFAULT 1073741824,
    created_at    TIMESTAMPTZ         DEFAULT NOW()
);

-- -- Optional: seed a test user
-- -- Generate hash:  python3 -c "import crypt; print(crypt.crypt('testpass', crypt.METHOD_SHA512))"
-- INSERT INTO users (email, password_hash)
-- VALUES ('test@bykiva.com', '$6$...')
-- ON CONFLICT DO NOTHING;
