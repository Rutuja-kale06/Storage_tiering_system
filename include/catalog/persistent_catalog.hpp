#pragma once

#include "catalog_interface.hpp"
#include <sqlite3.h>
#include <mutex>
#include <atomic>

class PersistentCatalog : public CatalogInterface {
public:
    PersistentCatalog();
    ~PersistentCatalog() override;

    // Non-copyable, non-movable
    PersistentCatalog(const PersistentCatalog&) = delete;
    PersistentCatalog& operator=(const PersistentCatalog&) = delete;

    // ── Lifecycle ────────────────────────────────────────────
    bool init(const std::string& db_path) override;
    void close() override;

    // ── CRUD ─────────────────────────────────────────────────
    bool add_file(const FileRecord& file) override;
    bool update_file(const FileRecord& file) override;
    bool upsert_file(const FileRecord& file) override;
    std::optional<FileRecord> get_file(const std::string& id) const override;
    std::optional<FileRecord> get_file_by_path(const std::string& path) const override;
    bool delete_file(const std::string& id) override;

    // ── Queries ──────────────────────────────────────────────
    std::vector<FileRecord> all_files() const override;
    std::vector<FileRecord> files_by_owner(const std::string& owner_id) const override;
    std::vector<FileRecord> files_by_tier(Tier t) const override;
    std::vector<FileRecord> files_needing_migration() const override;
    int file_count() const override;
    int file_count_by_tier(Tier t) const override;

    // ── Stats ────────────────────────────────────────────────
    TierStats stats_for_tier(Tier t) const override;
    std::array<TierStats, 4> all_tier_stats() const override;
    int64_t total_bytes() const override;
    double total_monthly_cost() const override;

    // ── Access Tracking ──────────────────────────────────────
    bool record_access(const std::string& id) override;
    bool record_write(const std::string& id) override;

    // ── Batch Operations ─────────────────────────────────────
    bool bulk_update_tier(const std::vector<std::string>& ids, Tier new_tier) override;
    bool bulk_set_pinned(const std::vector<std::string>& ids, bool pinned) override;

    // ── Migration History ────────────────────────────────────
    bool log_migration(const MigrationEvent& event) override;
    std::vector<MigrationEvent> recent_migrations(int n) const override;
    int64_t total_bytes_migrated() const override;
    int total_migrations() const override;
    bool clear_history() override;

    // ── Extent CRUD (sub-LUN tiering) ────────────────────────
    bool add_volume(const VolumeRecord& vol) override;
    bool update_volume(const VolumeRecord& vol) override;
    std::optional<VolumeRecord> get_volume(const std::string& id) const override;
    std::vector<VolumeRecord> all_volumes() const override;
    bool delete_volume(const std::string& id) override;

    bool add_extent(const ExtentRecord& extent) override;
    bool update_extent(const ExtentRecord& extent) override;
    bool upsert_extent(const ExtentRecord& extent) override;
    std::optional<ExtentRecord> get_extent(const std::string& id) const override;
    std::vector<ExtentRecord> extents_by_volume(const std::string& volume_id) const override;
    std::vector<ExtentRecord> extents_by_tier(Tier t) const override;
    std::vector<ExtentRecord> all_extents() const override;
    int extent_count() const override;
    int extent_count_by_tier(Tier t) const override;
    bool delete_extent(const std::string& id) override;

    bool log_io_sample(const std::string& extent_id, time_t timestamp,
                        int64_t iops_read, int64_t iops_write,
                        int64_t bytes_read, int64_t bytes_written,
                        double latency_us) override;
    std::vector<IOSample> recent_io_samples(const std::string& extent_id,
                                              int limit = 100) const override;

    bool log_heat_snapshot(const std::string& extent_id, time_t timestamp,
                            double heat_score, Tier tier) override;
    std::vector<std::tuple<time_t, double, Tier>> heat_history(
        const std::string& extent_id, int hours = 24) const override;

    bool log_extent_migration(const std::string& extent_id,
                               const std::string& volume_id,
                               Tier from_tier, Tier to_tier,
                               int64_t size_bytes,
                               const std::string& reason) override;
    std::vector<MigrationEvent> recent_extent_migrations(int n = 20) const override;

    // ── Maintenance ──────────────────────────────────────────
    bool vacuum() override;
    bool checkpoint() override;

private:
    sqlite3* db_ = nullptr;
    mutable std::mutex mtx_;
    std::atomic<bool> initialized_{false};

    // Prepared statements (cached for performance)
    sqlite3_stmt* stmt_add_file_        = nullptr;
    sqlite3_stmt* stmt_update_file_     = nullptr;
    sqlite3_stmt* stmt_upsert_file_     = nullptr;
    sqlite3_stmt* stmt_get_file_        = nullptr;
    sqlite3_stmt* stmt_get_file_by_path_= nullptr;
    sqlite3_stmt* stmt_delete_file_     = nullptr;
    sqlite3_stmt* stmt_record_access_      = nullptr;
    sqlite3_stmt* stmt_log_migration_      = nullptr;

    // Extent prepared statements
    sqlite3_stmt* stmt_add_extent_         = nullptr;
    sqlite3_stmt* stmt_update_extent_      = nullptr;
    sqlite3_stmt* stmt_upsert_extent_      = nullptr;
    sqlite3_stmt* stmt_get_extent_         = nullptr;
    sqlite3_stmt* stmt_extents_by_volume_  = nullptr;
    sqlite3_stmt* stmt_extents_by_tier_    = nullptr;
    sqlite3_stmt* stmt_all_extents_        = nullptr;
    sqlite3_stmt* stmt_delete_extent_      = nullptr;
    sqlite3_stmt* stmt_log_io_sample_      = nullptr;
    sqlite3_stmt* stmt_log_heat_snapshot_  = nullptr;
    sqlite3_stmt* stmt_log_extent_migrate_ = nullptr;

    // Volume prepared statements
    sqlite3_stmt* stmt_add_volume_         = nullptr;
    sqlite3_stmt* stmt_get_volume_         = nullptr;
    sqlite3_stmt* stmt_all_volumes_        = nullptr;
    sqlite3_stmt* stmt_delete_volume_      = nullptr;

    bool prepare_statements();
    void finalize_statements();
    bool exec_sql(const std::string& sql);
    FileRecord row_to_file_record(sqlite3_stmt* stmt) const;
    ExtentRecord row_to_extent(sqlite3_stmt* stmt) const;
    VolumeRecord row_to_volume(sqlite3_stmt* stmt) const;
    bool bind_extent(sqlite3_stmt* stmt, const ExtentRecord& e);

    static constexpr const char* DB_SCHEMA = R"(
        CREATE TABLE IF NOT EXISTS files (
            id            TEXT PRIMARY KEY,
            path          TEXT NOT NULL UNIQUE,
            extension     TEXT DEFAULT '',
            file_type     INTEGER NOT NULL DEFAULT 0,
            current_tier  INTEGER NOT NULL DEFAULT 0,
            target_tier   INTEGER NOT NULL DEFAULT 0,
            size_bytes    INTEGER NOT NULL DEFAULT 0,
            access_count  INTEGER NOT NULL DEFAULT 0,
            write_count   INTEGER NOT NULL DEFAULT 0,
            created_at    INTEGER NOT NULL,
            last_accessed INTEGER NOT NULL,
            last_modified INTEGER NOT NULL,
            migrate_count INTEGER NOT NULL DEFAULT 0,
            is_pinned     INTEGER NOT NULL DEFAULT 0,
            is_critical   INTEGER NOT NULL DEFAULT 0,
            score         REAL NOT NULL DEFAULT 0.0,
            owner_id      TEXT NOT NULL DEFAULT '',
            s3_bucket     TEXT NOT NULL DEFAULT '',
            s3_key        TEXT NOT NULL DEFAULT '',
            content_type  TEXT NOT NULL DEFAULT 'application/octet-stream',
            etag          TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_files_owner ON files(owner_id);
        CREATE INDEX IF NOT EXISTS idx_files_tier ON files(current_tier);
        CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);
        CREATE INDEX IF NOT EXISTS idx_files_last_accessed ON files(last_accessed);
        CREATE INDEX IF NOT EXISTS idx_files_access_count ON files(access_count);
        CREATE INDEX IF NOT EXISTS idx_files_s3_bucket ON files(s3_bucket);
        CREATE INDEX IF NOT EXISTS idx_files_s3_key ON files(s3_key);

        CREATE TABLE IF NOT EXISTS migration_history (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            file_id     TEXT NOT NULL,
            file_path   TEXT NOT NULL,
            from_tier   INTEGER NOT NULL,
            to_tier     INTEGER NOT NULL,
            size_bytes  INTEGER NOT NULL,
            reason      TEXT,
            timestamp   INTEGER NOT NULL,
            success     INTEGER NOT NULL DEFAULT 1,
            duration_ms REAL NOT NULL DEFAULT 0,
            FOREIGN KEY(file_id) REFERENCES files(id)
        );
        CREATE INDEX IF NOT EXISTS idx_migration_ts ON migration_history(timestamp);

        CREATE TABLE IF NOT EXISTS metadata (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS volumes (
            id             TEXT PRIMARY KEY,
            path           TEXT NOT NULL,
            total_size     INTEGER NOT NULL,
            extent_size    INTEGER NOT NULL DEFAULT 268435456,
            label          TEXT DEFAULT '',
            is_active      INTEGER NOT NULL DEFAULT 1,
            registered_at  INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS extents (
            id               TEXT PRIMARY KEY,
            volume_id        TEXT NOT NULL,
            offset_bytes     INTEGER NOT NULL,
            size_bytes       INTEGER NOT NULL,
            current_tier     INTEGER NOT NULL DEFAULT 0,
            target_tier      INTEGER NOT NULL DEFAULT 0,
            heat_score       REAL NOT NULL DEFAULT 0.0,
            prev_heat_score  REAL NOT NULL DEFAULT 0.0,
            iops_read        INTEGER NOT NULL DEFAULT 0,
            iops_write       INTEGER NOT NULL DEFAULT 0,
            bytes_read       INTEGER NOT NULL DEFAULT 0,
            bytes_written    INTEGER NOT NULL DEFAULT 0,
            sequential_ratio REAL NOT NULL DEFAULT 0.5,
            pe_cycles        INTEGER NOT NULL DEFAULT 0,
            is_pinned        INTEGER NOT NULL DEFAULT 0,
            sample_count     INTEGER NOT NULL DEFAULT 0,
            last_sampled     INTEGER NOT NULL DEFAULT 0,
            last_migrated    INTEGER NOT NULL DEFAULT 0,
            physical_path    TEXT NOT NULL DEFAULT '',
            physical_offset  INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY(volume_id) REFERENCES volumes(id)
        );
        CREATE INDEX IF NOT EXISTS idx_extents_volume ON extents(volume_id);
        CREATE INDEX IF NOT EXISTS idx_extents_tier ON extents(current_tier);
        CREATE INDEX IF NOT EXISTS idx_extents_heat ON extents(heat_score);

        CREATE TABLE IF NOT EXISTS io_samples (
            id             INTEGER PRIMARY KEY AUTOINCREMENT,
            extent_id      TEXT NOT NULL,
            timestamp      INTEGER NOT NULL,
            iops_read      INTEGER NOT NULL,
            iops_write     INTEGER NOT NULL,
            bytes_read     INTEGER NOT NULL DEFAULT 0,
            bytes_written  INTEGER NOT NULL DEFAULT 0,
            latency_us     REAL NOT NULL DEFAULT 0.0,
            FOREIGN KEY(extent_id) REFERENCES extents(id)
        );
        CREATE INDEX IF NOT EXISTS idx_io_samples_extent ON io_samples(extent_id, timestamp);

        CREATE TABLE IF NOT EXISTS heat_map_history (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            extent_id  TEXT NOT NULL,
            timestamp  INTEGER NOT NULL,
            heat_score REAL NOT NULL,
            tier       INTEGER NOT NULL,
            FOREIGN KEY(extent_id) REFERENCES extents(id)
        );
        CREATE INDEX IF NOT EXISTS idx_heat_history_extent ON heat_map_history(extent_id, timestamp);

        CREATE TABLE IF NOT EXISTS extent_migration_history (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            extent_id   TEXT NOT NULL,
            volume_id   TEXT NOT NULL,
            from_tier   INTEGER NOT NULL,
            to_tier     INTEGER NOT NULL,
            size_bytes  INTEGER NOT NULL,
            reason      TEXT,
            timestamp   INTEGER NOT NULL,
            success     INTEGER NOT NULL DEFAULT 1,
            duration_ms REAL NOT NULL DEFAULT 0,
            FOREIGN KEY(extent_id) REFERENCES extents(id)
        );
        CREATE INDEX IF NOT EXISTS idx_extent_migrate_ts ON extent_migration_history(timestamp);

        PRAGMA journal_mode = WAL;
        PRAGMA wal_autocheckpoint = 1000;
        PRAGMA foreign_keys = ON;
    )";

    bool verify_schema();
};
