#pragma once

#include "migrator_interface.hpp"
#include <string>

struct SmbShareConfig {
    std::string host;
    std::string share;
    std::string mount_point;
    std::string username;
    std::string password;
    std::string domain;
    int tier = 2;
    bool enabled = true;
    std::string label;
};

class SmbMigrator : public Migrator {
public:
    SmbMigrator(const SmbShareConfig& config, const std::string& tier_base);

    MigratorType type() const override { return MigratorType::SMB; }
    std::string name() const override { return "smb_" + label_; }

    MigrateResult migrate(FileRecord& file, Tier target_tier) override;
    bool rollback(FileRecord& file, const std::string& original_path) override;
    bool verify(const FileRecord& file) override;
    bool can_migrate(const FileRecord& file, Tier target_tier) override;

    bool mount_share();
    bool unmount_share();
    bool is_connected() const;
    std::string unc_path() const;

    const SmbShareConfig& config() const { return config_; }

private:
    SmbShareConfig config_;
    std::string label_;
    std::string base_dir_;
    std::string share_unc_;
    bool connected_ = false;
    int retry_count_ = 3;
    int retry_delay_ms_ = 2000;

    std::string tier_subdir(Tier t) const;
    bool ensure_tier_dir(Tier t);
    std::string unique_path(const std::string& dir, const std::string& filename) const;
    bool try_reconnect();
};
