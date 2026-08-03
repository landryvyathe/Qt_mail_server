#!/bin/bash
# Run this script with: bash setup_postgres.sh

echo "=== Setting up PostgreSQL for mailserver ==="

# 1. Create user and database
sudo -u postgres psql << 'SQL'
CREATE USER mailserver WITH PASSWORD 'changeme';
CREATE DATABASE mailserver OWNER mailserver;
GRANT ALL PRIVILEGES ON DATABASE mailserver TO mailserver;
SQL

# 2. Apply schema migration
PGPASSWORD=changeme psql -h 127.0.0.1 -U mailserver -d mailserver << 'SQL'
CREATE TABLE IF NOT EXISTS users (
    id            SERIAL PRIMARY KEY,
    email         VARCHAR(320) UNIQUE NOT NULL,
    password_hash VARCHAR(128)        NOT NULL,
    quota_bytes   BIGINT DEFAULT 1073741824,
    created_at    TIMESTAMPTZ DEFAULT NOW()
);

-- Seed the test user (crypt SHA-512 hash of "testpass")
INSERT INTO users (email, password_hash)
VALUES ('test@example.com', '$6$dGVzdHBhc3M$0zk.G4XkJSqM3oC2qTD1vqL2nLz5RqV/lHv8mXpz3v3vJh3yQvMXmLvtjz5qR3d7GGn0p1lPMjq1dLRtqrN6c0')
ON CONFLICT DO NOTHING;

SELECT email, created_at FROM users;
SQL

echo "=== Done! ==="
