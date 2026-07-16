-- Storage Tiering System - Supabase Schema
-- Run this in Supabase SQL Editor

-- Users table (replaces users.json)
CREATE TABLE IF NOT EXISTS public.users (
    id            TEXT PRIMARY KEY,
    username      TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    display_name  TEXT DEFAULT '',
    role          TEXT DEFAULT 'user' CHECK (role IN ('admin', 'operator', 'viewer', 'user')),
    created_at    BIGINT DEFAULT 0,
    last_login    BIGINT DEFAULT 0
);

-- Files table (per-user file records)
CREATE TABLE IF NOT EXISTS public.files (
    id            TEXT PRIMARY KEY,
    path          TEXT NOT NULL,
    extension     TEXT DEFAULT '',
    file_type     INTEGER DEFAULT 0,
    current_tier  INTEGER DEFAULT 0,
    target_tier   INTEGER DEFAULT 0,
    size_bytes    BIGINT DEFAULT 0,
    access_count  INTEGER DEFAULT 0,
    write_count   INTEGER DEFAULT 0,
    created_at    BIGINT DEFAULT 0,
    last_accessed BIGINT DEFAULT 0,
    last_modified BIGINT DEFAULT 0,
    migrate_count INTEGER DEFAULT 0,
    is_pinned     BOOLEAN DEFAULT FALSE,
    is_critical   BOOLEAN DEFAULT FALSE,
    score         REAL DEFAULT 0.0,
    owner_id      TEXT NOT NULL DEFAULT '',
    s3_bucket     TEXT DEFAULT '',
    s3_key        TEXT DEFAULT '',
    content_type  TEXT DEFAULT 'application/octet-stream',
    etag          TEXT DEFAULT ''
);

-- Migration history
CREATE TABLE IF NOT EXISTS public.migration_history (
    id          BIGSERIAL PRIMARY KEY,
    file_id     TEXT NOT NULL,
    file_path   TEXT NOT NULL,
    from_tier   INTEGER NOT NULL,
    to_tier     INTEGER NOT NULL,
    size_bytes  BIGINT NOT NULL,
    reason      TEXT,
    timestamp   BIGINT NOT NULL,
    success     BOOLEAN DEFAULT TRUE,
    duration_ms REAL DEFAULT 0
);

-- Indexes
CREATE INDEX IF NOT EXISTS idx_files_owner ON public.files(owner_id);
CREATE INDEX IF NOT EXISTS idx_files_tier ON public.files(current_tier);
CREATE INDEX IF NOT EXISTS idx_files_path ON public.files(path);

-- Helper function (must exist before policies reference it)
CREATE OR REPLACE FUNCTION public.current_user_id()
RETURNS TEXT
LANGUAGE SQL STABLE
AS $$
    SELECT COALESCE(
        current_setting('request.jwt.claim.sub', TRUE),
        ''
    );
$$;

-- Row Level Security
ALTER TABLE public.users ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.files ENABLE ROW LEVEL SECURITY;

-- Users policies
CREATE POLICY "Users can read own data" ON public.users
    FOR SELECT USING (id = current_user_id() OR role = 'admin');

CREATE POLICY "Users can update own data" ON public.users
    FOR UPDATE USING (id = current_user_id());

-- Files policies: users see only their own files
CREATE POLICY "Users can read own files" ON public.files
    FOR SELECT USING (owner_id = current_user_id());

CREATE POLICY "Users can insert own files" ON public.files
    FOR INSERT WITH CHECK (owner_id = current_user_id());

CREATE POLICY "Users can update own files" ON public.files
    FOR UPDATE USING (owner_id = current_user_id());

CREATE POLICY "Users can delete own files" ON public.files
    FOR DELETE USING (owner_id = current_user_id());

-- Admin can see all
CREATE POLICY "Admin can read all files" ON public.files
    FOR SELECT USING (current_user_id() IN (
        SELECT id FROM public.users WHERE role = 'admin'
    ));
