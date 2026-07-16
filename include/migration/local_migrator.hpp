#pragma once

#include "migrator_interface.hpp"
#include <string>
#include <unordered_map>

class LocalMigrator : public Migrator {
public:
    explicit LocalMigrator(const std::string& base_directory);

    MigratorType type() const override { return MigratorType::LOCAL; }
    std::string name() const override { return "local"; }

    // Create tier subdirectories (HOT/, WARM/, COLD/, ARCHIVE/)
    bool create_tier_directories();

    MigrateResult migrate(FileRecord& file, Tier target_tier) override;
    bool rollback(FileRecord& file, const std::string& original_path) override;
    bool verify(const FileRecord& file) override;
    bool can_migrate(const FileRecord& file, Tier target_tier) override;

    // Track original paths for rollback
    struct BackupRecord {
        std::string original_path;
        Tier        original_tier;
        std::string backup_path;  // copy made before migration
        time_t      timestamp;
    };
    const std::unordered_map<std::string, BackupRecord>& backups() const { return backups_; }

    // Backup retention
    void set_max_backup_age_days(int days) { max_backup_age_days_ = days; }
    void set_max_backup_count(int n) { max_backup_count_ = n; }
    void cleanup_old_backups();

private:
    std::string base_dir_;
    int max_backup_age_days_ = 90;
    int max_backup_count_ = 1000;
    std::unordered_map<std::string, BackupRecord> backups_;

    std::string tier_directory(Tier t) const;
    std::string unique_path(const std::string& dir, const std::string& filename) const;
};
