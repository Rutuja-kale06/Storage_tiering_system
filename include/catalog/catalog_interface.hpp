#pragma once

#include "core/file_record.hpp"
#include "tiering/extent.hpp"
#include <vector>
#include <string>
#include <optional>
#include <array>

class CatalogInterface {
public:
    virtual ~CatalogInterface() = default;

    // ── Lifecycle ────────────────────────────────────────────
    virtual bool init(const std::string& db_path) = 0;
    virtual void close() = 0;

    // ── CRUD ─────────────────────────────────────────────────
    virtual bool add_file(const FileRecord& file) = 0;
    virtual bool update_file(const FileRecord& file) = 0;
    virtual bool upsert_file(const FileRecord& file) = 0;
    virtual std::optional<FileRecord> get_file(const std::string& id) const = 0;
    virtual std::optional<FileRecord> get_file_by_path(const std::string& path) const = 0;
    virtual bool delete_file(const std::string& id) = 0;

    // ── Queries ──────────────────────────────────────────────
    virtual std::vector<FileRecord> all_files() const = 0;
    virtual std::vector<FileRecord> files_by_owner(const std::string& owner_id) const = 0;
    virtual std::vector<FileRecord> files_by_tier(Tier t) const = 0;
    virtual std::vector<FileRecord> files_needing_migration() const = 0;
    virtual int file_count() const = 0;
    virtual int file_count_by_tier(Tier t) const = 0;

    // ── Stats ────────────────────────────────────────────────
    virtual TierStats stats_for_tier(Tier t) const = 0;
    virtual std::array<TierStats, 4> all_tier_stats() const = 0;
    virtual int64_t total_bytes() const = 0;
    virtual double total_monthly_cost() const = 0;

    // ── Access Tracking ──────────────────────────────────────
    virtual bool record_access(const std::string& id) = 0;
    virtual bool record_write(const std::string& id) = 0;

    // ── Batch Operations ─────────────────────────────────────
    virtual bool bulk_update_tier(const std::vector<std::string>& ids, Tier new_tier) = 0;
    virtual bool bulk_set_pinned(const std::vector<std::string>& ids, bool pinned) = 0;

    // ── Migration History ────────────────────────────────────
    virtual bool log_migration(const MigrationEvent& event) = 0;
    virtual std::vector<MigrationEvent> recent_migrations(int n) const = 0;
    virtual int64_t total_bytes_migrated() const = 0;
    virtual int total_migrations() const = 0;
    virtual bool clear_history() = 0;

    // ── Extent CRUD (sub-LUN tiering) ────────────────────────
    virtual bool add_volume(const VolumeRecord& vol) = 0;
    virtual bool update_volume(const VolumeRecord& vol) = 0;
    virtual std::optional<VolumeRecord> get_volume(const std::string& id) const = 0;
    virtual std::vector<VolumeRecord> all_volumes() const = 0;
    virtual bool delete_volume(const std::string& id) = 0;

    virtual bool add_extent(const ExtentRecord& extent) = 0;
    virtual bool update_extent(const ExtentRecord& extent) = 0;
    virtual bool upsert_extent(const ExtentRecord& extent) = 0;
    virtual std::optional<ExtentRecord> get_extent(const std::string& id) const = 0;
    virtual std::vector<ExtentRecord> extents_by_volume(const std::string& volume_id) const = 0;
    virtual std::vector<ExtentRecord> extents_by_tier(Tier t) const = 0;
    virtual std::vector<ExtentRecord> all_extents() const = 0;
    virtual int extent_count() const = 0;
    virtual int extent_count_by_tier(Tier t) const = 0;
    virtual bool delete_extent(const std::string& id) = 0;

    // ── IO Samples ────────────────────────────────────────────
    virtual bool log_io_sample(const std::string& extent_id, time_t timestamp,
                                int64_t iops_read, int64_t iops_write,
                                int64_t bytes_read, int64_t bytes_written,
                                double latency_us) = 0;
    virtual std::vector<IOSample> recent_io_samples(const std::string& extent_id,
                                                      int limit = 100) const = 0;

    // ── Heat Map History ──────────────────────────────────────
    virtual bool log_heat_snapshot(const std::string& extent_id, time_t timestamp,
                                    double heat_score, Tier tier) = 0;
    virtual std::vector<std::tuple<time_t, double, Tier>> heat_history(
        const std::string& extent_id, int hours = 24) const = 0;

    // ── Extent Migration History ──────────────────────────────
    virtual bool log_extent_migration(const std::string& extent_id,
                                       const std::string& volume_id,
                                       Tier from_tier, Tier to_tier,
                                       int64_t size_bytes,
                                       const std::string& reason) = 0;
    virtual std::vector<MigrationEvent> recent_extent_migrations(int n = 20) const = 0;

    // ── Maintenance ──────────────────────────────────────────
    virtual bool vacuum() = 0;
    virtual bool checkpoint() = 0;
};
