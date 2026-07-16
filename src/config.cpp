#include "config.hpp"
#include "logger.hpp"
#include <fstream>
#include <filesystem>
#include <cctype>
#include <cstdlib>

namespace fs = std::filesystem;

static std::string sanitize_path(const std::string& path) {
    if (path.empty()) return path;
    try {
        return fs::weakly_canonical(path).string();
    } catch (...) {
        return fs::absolute(path).string();
    }
}

bool ConfigManager::load(const std::string& json_path) {
    path_ = json_path;
    return reload();
}

bool ConfigManager::reload() {
    try {
        std::ifstream f(path_);
        if (!f.is_open()) {
            LOG_WARN("Config", "Cannot open " + path_ + ", using defaults");
            return false;
        }

        nlohmann::json cfg;
        f >> cfg;

        if (cfg.contains("db_path"))             config_.db_path = sanitize_path(cfg["db_path"].get<std::string>());
        if (cfg.contains("scan_root"))           config_.scan_root = sanitize_path(cfg["scan_root"].get<std::string>());
        if (cfg.contains("tier_base"))           config_.tier_base = sanitize_path(cfg["tier_base"].get<std::string>());
        if (cfg.contains("recursive_scan"))      config_.recursive_scan = cfg["recursive_scan"].get<bool>();

        if (cfg.contains("policy_config_path"))   config_.policy_config_path = sanitize_path(cfg["policy_config_path"].get<std::string>());
        if (cfg.contains("lifecycle_rules_path")) config_.lifecycle_rules_path = sanitize_path(cfg["lifecycle_rules_path"].get<std::string>());

        if (cfg.contains("enable_file_watcher"))  config_.enable_file_watcher = cfg["enable_file_watcher"].get<bool>();
        if (cfg.contains("enable_access_tracker")) config_.enable_access_tracker = cfg["enable_access_tracker"].get<bool>();
        if (cfg.contains("watcher_poll_interval_ms")) config_.watcher_poll_interval_ms = cfg["watcher_poll_interval_ms"].get<int>();

        if (cfg.contains("migration")) {
            auto& m = cfg["migration"];
            if (m.contains("max_bandwidth_mbps"))     config_.max_bandwidth_mbps = m["max_bandwidth_mbps"].get<double>();
            if (m.contains("max_concurrent_jobs"))    config_.max_concurrent_jobs = m["max_concurrent_jobs"].get<int>();
            if (m.contains("maintenance_window"))     config_.maintenance_window = m["maintenance_window"].get<bool>();
            if (m.contains("window_start_hour"))      config_.window_start_hour = m["window_start_hour"].get<int>();
            if (m.contains("window_end_hour"))        config_.window_end_hour = m["window_end_hour"].get<int>();
            if (m.contains("verify_after_migration")) config_.verify_after_migration = m["verify_after_migration"].get<bool>();
            if (m.contains("create_backups"))         config_.create_backups = m["create_backups"].get<bool>();
        }

        if (cfg.contains("rest_api")) {
            auto& r = cfg["rest_api"];
            if (r.contains("enabled"))      config_.enable_rest_api = r["enabled"].get<bool>();
            if (r.contains("port"))         config_.rest_api_port = r["port"].get<int>();
            if (r.contains("api_key"))      config_.api_key = r["api_key"].get<std::string>();
            if (r.contains("jwt_secret"))   config_.jwt_secret = r["jwt_secret"].get<std::string>();
        }

        if (cfg.contains("s3_api")) {
            auto& s3 = cfg["s3_api"];
            if (s3.contains("enabled"))    config_.enable_s3_api = s3["enabled"].get<bool>();
            if (s3.contains("port"))       config_.s3_api_port = s3["port"].get<int>();
            if (s3.contains("base_path"))  config_.s3_base_path = s3["base_path"].get<std::string>();
        }

        if (cfg.contains("logging")) {
            auto& l = cfg["logging"];
            if (l.contains("file"))  config_.log_file = l["file"].get<std::string>();
            if (l.contains("level")) config_.log_level = l["level"].get<std::string>();
        }

        if (cfg.contains("daemon")) {
            auto& d = cfg["daemon"];
            if (d.contains("enabled"))            config_.run_as_daemon = d["enabled"].get<bool>();
            if (d.contains("cycle_interval_min")) config_.cycle_interval_min = d["cycle_interval_min"].get<int>();
        }

        if (cfg.contains("drive_detector")) {
            auto& dd = cfg["drive_detector"];
            if (dd.contains("enabled"))             config_.enable_drive_detector = dd["enabled"].get<bool>();
            if (dd.contains("poll_interval_sec"))   config_.drive_poll_interval_sec = dd["poll_interval_sec"].get<int>();
        }

        if (cfg.contains("dashboard")) {
            config_.enable_dashboard = cfg["dashboard"].value("enabled", true);
        }

        // Extent-tiering (sub-LUN)
        if (cfg.contains("extent_tiering")) {
            auto& et = cfg["extent_tiering"];
            config_.extent_tiering_enabled          = et.value("enabled", false);
            config_.extent_size_bytes               = et.value("extent_size_mb", 256) * 1024LL * 1024;
            config_.extent_ema_alpha                = et.value("ema_alpha", 0.3);
            config_.extent_sampling_interval_seconds = et.value("sampling_interval_seconds", 5);
            config_.extent_analysis_interval_minutes = et.value("analysis_interval_minutes", 60);
            config_.extent_relocation_window_start   = et.value("relocation_window_start_hour", 2);
            config_.extent_relocation_window_end     = et.value("relocation_window_end_hour", 5);
            config_.extent_hysteresis_band           = et.value("hysteresis_band", 5.0);
            config_.extent_expected_spike_seconds    = et.value("expected_spike_seconds", 300.0);
            config_.extent_sequential_threshold      = et.value("sequential_threshold", 0.7);
            config_.extent_wear_limit_pe_cycles      = et.value("wear_limit_pe_cycles", 100000);
            config_.extent_max_samples_per_extent    = et.value("max_samples_per_extent", 1000);
            config_.extent_bandwidth_mbps            = et.value("bandwidth_mbps", 100.0);
            config_.extent_wear_penalty_per_gb       = et.value("wear_penalty_per_gb", 0.5);

            if (et.contains("tier_watermarks")) {
                auto& tw = et["tier_watermarks"];
                if (tw.contains("HOT")) {
                    config_.extent_hot_max_capacity_percent = tw["HOT"].value("max_capacity_percent", 85.0);
                    config_.extent_hot_iops_capacity        = tw["HOT"].value("iops_capacity", 40000);
                }
                if (tw.contains("WARM")) {
                    config_.extent_warm_max_capacity_percent = tw["WARM"].value("max_capacity_percent", 90.0);
                    config_.extent_warm_iops_capacity        = tw["WARM"].value("iops_capacity", 2000);
                }
                if (tw.contains("COLD")) {
                    config_.extent_cold_max_capacity_percent = tw["COLD"].value("max_capacity_percent", 95.0);
                    config_.extent_cold_iops_capacity        = tw["COLD"].value("iops_capacity", 320);
                }
                if (tw.contains("ARCHIVE")) {
                    config_.extent_archive_max_capacity_percent = tw["ARCHIVE"].value("max_capacity_percent", 100.0);
                    config_.extent_archive_iops_capacity        = tw["ARCHIVE"].value("iops_capacity", 100);
                }
            }

            if (et.contains("write_coalescing")) {
                auto& wc = et["write_coalescing"];
                config_.extent_write_coalescing_enabled  = wc.value("enabled", true);
                config_.extent_write_coalescing_flush_ms = wc.value("flush_interval_ms", 100);
                config_.extent_write_coalescing_buffer_mb = wc.value("max_buffer_size_mb", 64);
            }

            if (et.contains("wear_leveling")) {
                auto& wl = et["wear_leveling"];
                config_.extent_wear_leveling_enabled    = wl.value("enabled", true);
                config_.extent_wear_balance_threshold    = wl.value("target_pe_balance_threshold", 0.2);
            }
        }

        // Supabase
        if (cfg.contains("supabase")) {
            auto& sb = cfg["supabase"];
            config_.supabase_enabled = sb.value("enabled", false);
            config_.supabase_url     = sb.value("url", "");
            config_.supabase_anon_key = sb.value("anon_key", "");
        }
        // Environment variables override config file
        const char* env_url  = std::getenv("SUPABASE_URL");
        const char* env_key  = std::getenv("SUPABASE_ANON_KEY");
        if (env_url  && *env_url)  config_.supabase_url = env_url;
        if (env_key  && *env_key)  config_.supabase_anon_key = env_key;
        if (!config_.supabase_url.empty() && !config_.supabase_anon_key.empty())
            config_.supabase_enabled = true;

        // Parse SMB shares
        config_.smb_shares.clear();
        if (cfg.contains("smb") && cfg["smb"].contains("shares")) {
            for (const auto& s : cfg["smb"]["shares"]) {
                AppConfig::SmbShare share;
                share.host        = s.value("host", "");
                share.share       = s.value("share", "");
                share.mount_point = s.value("mount_point", "");
                share.username    = s.value("username", "");
                share.password    = s.value("password", "");
                share.domain      = s.value("domain", "");
                share.tier        = s.value("tier", 2);
                share.enabled     = s.value("enabled", true);
                share.label       = s.value("label", "");
                if (!share.host.empty() && !share.share.empty())
                    config_.smb_shares.push_back(share);
            }
            LOG_INFO("Config", "Loaded " + std::to_string(config_.smb_shares.size()) + " SMB share(s)");
        }

        LOG_INFO("Config", "Configuration loaded from " + path_);
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("Config", std::string("Failed to load config: ") + e.what());
        return false;
    }
}

bool ConfigManager::save(const std::string& json_path) const {
    try {
        nlohmann::json cfg;

        cfg["db_path"]               = config_.db_path;
        cfg["scan_root"]             = config_.scan_root;
        cfg["tier_base"]             = config_.tier_base;
        cfg["recursive_scan"]        = config_.recursive_scan;
        cfg["policy_config_path"]    = config_.policy_config_path;
        cfg["lifecycle_rules_path"]  = config_.lifecycle_rules_path;
        cfg["enable_file_watcher"]   = config_.enable_file_watcher;
        cfg["enable_access_tracker"] = config_.enable_access_tracker;
        cfg["watcher_poll_interval_ms"] = config_.watcher_poll_interval_ms;

        cfg["migration"]["max_bandwidth_mbps"]     = config_.max_bandwidth_mbps;
        cfg["migration"]["max_concurrent_jobs"]    = config_.max_concurrent_jobs;
        cfg["migration"]["maintenance_window"]     = config_.maintenance_window;
        cfg["migration"]["window_start_hour"]      = config_.window_start_hour;
        cfg["migration"]["window_end_hour"]        = config_.window_end_hour;
        cfg["migration"]["verify_after_migration"] = config_.verify_after_migration;
        cfg["migration"]["create_backups"]         = config_.create_backups;

        cfg["rest_api"]["enabled"] = config_.enable_rest_api;
        cfg["rest_api"]["port"]    = config_.rest_api_port;
        cfg["rest_api"]["api_key"]    = config_.api_key;
        cfg["rest_api"]["jwt_secret"] = config_.jwt_secret;

        cfg["s3_api"]["enabled"]   = config_.enable_s3_api;
        cfg["s3_api"]["port"]      = config_.s3_api_port;
        cfg["s3_api"]["base_path"] = config_.s3_base_path;

        cfg["logging"]["file"]  = config_.log_file;
        cfg["logging"]["level"] = config_.log_level;

        cfg["daemon"]["enabled"]            = config_.run_as_daemon;
        cfg["daemon"]["cycle_interval_min"] = config_.cycle_interval_min;

        cfg["drive_detector"]["enabled"]           = config_.enable_drive_detector;
        cfg["drive_detector"]["poll_interval_sec"] = config_.drive_poll_interval_sec;

        cfg["dashboard"]["enabled"] = config_.enable_dashboard;

        cfg["extent_tiering"]["enabled"]                    = config_.extent_tiering_enabled;
        cfg["extent_tiering"]["extent_size_mb"]             = static_cast<int>(config_.extent_size_bytes / (1024*1024));
        cfg["extent_tiering"]["ema_alpha"]                  = config_.extent_ema_alpha;
        cfg["extent_tiering"]["sampling_interval_seconds"]  = config_.extent_sampling_interval_seconds;
        cfg["extent_tiering"]["analysis_interval_minutes"]  = config_.extent_analysis_interval_minutes;
        cfg["extent_tiering"]["hysteresis_band"]            = config_.extent_hysteresis_band;
        cfg["extent_tiering"]["sequential_threshold"]       = config_.extent_sequential_threshold;
        cfg["extent_tiering"]["wear_limit_pe_cycles"]       = config_.extent_wear_limit_pe_cycles;
        cfg["extent_tiering"]["bandwidth_mbps"]             = config_.extent_bandwidth_mbps;

        cfg["supabase"]["enabled"]   = config_.supabase_enabled;
        cfg["supabase"]["url"]       = config_.supabase_url;
        cfg["supabase"]["anon_key"]  = config_.supabase_anon_key;

        std::ofstream f(json_path);
        f << cfg.dump(2);
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("Config", std::string("Failed to save config: ") + e.what());
        return false;
    }
}

ConfigManager& ConfigManager::instance() {
    static ConfigManager inst;
    return inst;
}
