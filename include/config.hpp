#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct AppConfig {
    std::string db_path = "storage_tiering.db";
    std::string scan_root = ".";
    std::string tier_base = ".";
    bool        recursive_scan = true;

    std::string policy_config_path = "config/tiering_config.json";
    std::string lifecycle_rules_path = "config/policy_rules.yaml";

    bool enable_file_watcher  = true;
    bool enable_access_tracker = false;
    int  watcher_poll_interval_ms = 5000;

    double max_bandwidth_mbps     = 100.0;
    int    max_concurrent_jobs    = 4;
    bool   maintenance_window     = false;
    int    window_start_hour      = 2;
    int    window_end_hour        = 5;
    bool   verify_after_migration = true;
    bool   create_backups         = true;

    bool   enable_rest_api   = true;
    int    rest_api_port     = 3000;
    std::string api_key      = "";
    std::string jwt_secret   = "";

    bool   enable_s3_api   = false;
    int    s3_api_port     = 3001;
    std::string s3_base_path = ".";

    std::string log_file      = "";
    std::string log_level     = "info";

    bool   run_as_daemon      = false;
    int    cycle_interval_min = 60;

    bool   enable_drive_detector = true;
    int    drive_poll_interval_sec = 10;

    // Supabase
    bool   supabase_enabled   = false;
    std::string supabase_url  = "";
    std::string supabase_anon_key = "";

    struct SmbShare {
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
    std::vector<SmbShare> smb_shares;

    bool   enable_dashboard   = true;

    // ── Extent Tiering (sub-LUN) ────────────────────────────
    bool   extent_tiering_enabled            = false;
    int64_t extent_size_bytes                = 268435456; // 256 MB
    double extent_ema_alpha                  = 0.3;
    int    extent_sampling_interval_seconds   = 5;
    int    extent_analysis_interval_minutes   = 60;
    int    extent_relocation_window_start     = 2;
    int    extent_relocation_window_end       = 5;
    double extent_hysteresis_band             = 5.0;
    double extent_expected_spike_seconds      = 300.0;
    double extent_sequential_threshold        = 0.7;
    int64_t extent_wear_limit_pe_cycles       = 100000;
    int    extent_max_samples_per_extent      = 1000;
    double extent_bandwidth_mbps              = 100.0;
    double extent_wear_penalty_per_gb         = 0.5;
    double extent_hot_max_capacity_percent    = 85.0;
    int64_t extent_hot_iops_capacity          = 40000;
    double extent_warm_max_capacity_percent   = 90.0;
    int64_t extent_warm_iops_capacity         = 2000;
    double extent_cold_max_capacity_percent   = 95.0;
    int64_t extent_cold_iops_capacity         = 320;
    double extent_archive_max_capacity_percent = 100.0;
    int64_t extent_archive_iops_capacity      = 100;
    bool   extent_write_coalescing_enabled    = true;
    int    extent_write_coalescing_flush_ms   = 100;
    int    extent_write_coalescing_buffer_mb  = 64;
    bool   extent_wear_leveling_enabled       = true;
    double extent_wear_balance_threshold      = 0.2;
};

class ConfigManager {
public:
    bool load(const std::string& json_path);
    bool save(const std::string& json_path) const;
    bool reload();

    AppConfig& config() { return config_; }
    const AppConfig& config() const { return config_; }

    static ConfigManager& instance();

private:
    AppConfig config_;
    std::string path_;
};
