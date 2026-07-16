#include "catalog/persistent_catalog.hpp"
#include "policy/policy_engine.hpp"
#include "migration/migration_engine.hpp"
#include "migration/local_migrator.hpp"
#include "migration/smb_migrator.hpp"
#include "scanner/real_scanner.hpp"
#include "monitoring/file_watcher.hpp"
#include "monitoring/access_tracker.hpp"
#include "monitoring/drive_detector.hpp"
#include "api/rest_server.hpp"
#include "api/s3_server.hpp"
#include "api/supabase_client.hpp"
#include "dashboard.hpp"
#include "config.hpp"
#include "logger.hpp"

#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

std::atomic<bool> g_shutdown{false};

void signal_handler(int) {
    g_shutdown.store(true);
}

void run_daemon_loop(std::shared_ptr<MigrationEngine> engine,
                     std::shared_ptr<CatalogInterface> catalog,
                     std::shared_ptr<PolicyEngine> policy)
{
    int interval_min = ConfigManager::instance().config().cycle_interval_min;
    LOG_INFO("Daemon", "Auto-cycle every " + std::to_string(interval_min) + " minutes");

    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::minutes(interval_min));
        if (g_shutdown.load()) break;

        LOG_INFO("Daemon", "Running scheduled tiering cycle");
        auto result = engine->run_cycle();

        if (result.migrated > 0) {
            LOG_INFO("Daemon", "Cycle complete: " + std::to_string(result.migrated)
                     + " migrated, $" + std::to_string(result.estimated_monthly_savings)
                     + "/mo savings");
        } else {
            LOG_DEBUG("Daemon", "Cycle complete: no migrations needed");
        }
    }
}

int main(int argc, char* argv[]) {
    // ── Parse CLI args ───────────────────────────────────────
    std::string auto_scan_dir;
    bool run_once = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--once")       run_once = true;
        else if (arg == "--scan" && i + 1 < argc) auto_scan_dir = argv[++i];
    }

    // ── Init components ──────────────────────────────────────
    auto catalog = std::make_shared<PersistentCatalog>();
    auto policy  = std::make_shared<PolicyEngine>();
    auto engine  = std::make_shared<MigrationEngine>(catalog, policy);
    auto config  = &ConfigManager::instance();

    // Load config
    config->load("config/tiering_config.json");
    auto& cfg = config->config();

    // Init logger
    Logger::instance().init(cfg.log_file);
    if (cfg.log_level == "trace")     Logger::instance().set_level(LogLevel::TRACE);
    else if (cfg.log_level == "debug") Logger::instance().set_level(LogLevel::DEBUG);
    else if (cfg.log_level == "warn")  Logger::instance().set_level(LogLevel::WARN);
    else if (cfg.log_level == "error") Logger::instance().set_level(LogLevel::ERROR);

    LOG_INFO("Main", "Starting Storage Tiering System v2.0");

    // Init catalog (SQLite)
    if (!catalog->init(cfg.db_path)) {
        LOG_ERROR("Main", "Failed to initialize database: " + cfg.db_path);
        std::cerr << RED << "  [FATAL] Could not open database: "
                  << cfg.db_path << RESET << "\n";
        return 1;
    }
    LOG_INFO("Main", "Database opened: " + cfg.db_path);

    // Load policy
    if (!cfg.policy_config_path.empty()) {
        if (policy->load_config(cfg.policy_config_path)) {
            LOG_INFO("Main", "Policy config loaded from " + cfg.policy_config_path);
        } else {
            LOG_WARN("Main", "Using default policy configuration");
        }
    }
    if (!cfg.lifecycle_rules_path.empty()) {
        if (policy->load_lifecycle_rules(cfg.lifecycle_rules_path)) {
            LOG_INFO("Main", "Lifecycle rules loaded from " + cfg.lifecycle_rules_path);
        }
    }

    // Register local migrator
    engine->register_migrator(std::make_unique<LocalMigrator>(cfg.tier_base));

    // Register SMB migrators for each configured share
    for (const auto& smb_cfg : cfg.smb_shares) {
        if (!smb_cfg.enabled) continue;
        SmbShareConfig sc;
        sc.host        = smb_cfg.host;
        sc.share       = smb_cfg.share;
        sc.mount_point = smb_cfg.mount_point;
        sc.username    = smb_cfg.username;
        sc.password    = smb_cfg.password;
        sc.domain      = smb_cfg.domain;
        sc.tier        = smb_cfg.tier;
        sc.enabled     = smb_cfg.enabled;
        sc.label       = smb_cfg.label;
        auto migrator = std::make_unique<SmbMigrator>(sc, cfg.tier_base);
        if (migrator->mount_share()) {
            engine->register_migrator(std::move(migrator));
            LOG_INFO("Main", "SMB share " + smb_cfg.host + "\\" + smb_cfg.share +
                     " mapped to tier " + tier_name(static_cast<Tier>(smb_cfg.tier)));
        } else {
            LOG_WARN("Main", "Failed to mount SMB share " + smb_cfg.host + "\\" + smb_cfg.share);
        }
    }

    // ── Drive Detector (hot-plug) ──────────────────────────
    std::unique_ptr<DriveDetector> drive_detector;
    if (cfg.enable_drive_detector) {
        drive_detector = std::make_unique<DriveDetector>();
        drive_detector->set_poll_interval_sec(cfg.drive_poll_interval_sec);
        drive_detector->start([&](const DriveInfo& di, bool added) {
            if (!added) return;
            Tier tier = drive_hardware_to_tier(di.hardware_type);
            LOG_INFO("DriveDetector", "Auto-scanning " + di.mount_point +
                     " (" + di.label + ") -> " + tier_name(tier));

            RealScanner scanner;
            scanner.set_root(di.mount_point);
            scanner.set_recursive(true);
            scanner.set_max_files(5000);
            scanner.set_default_tier(tier);
            auto result = scanner.scan(*catalog, false);

            if (result.added > 0)
                LOG_INFO("DriveDetector", "Added " + std::to_string(result.added) +
                         " files from " + di.mount_point + " to " + tier_name(tier) + " tier");
        });
        LOG_INFO("Main", "Drive detector started (poll every " +
                 std::to_string(cfg.drive_poll_interval_sec) + "s)");

        // Scan all existing drives at startup to populate the catalog
        auto existing_drives = DriveDetector::enumerate_drives();
        int total_added = 0;
        for (const auto& di : existing_drives) {
            Tier tier = drive_hardware_to_tier(di.hardware_type);
            LOG_INFO("Main", "Startup scan: " + di.mount_point +
                     " (" + di.label + ") -> " + tier_name(tier));
            RealScanner scanner;
            scanner.set_root(di.mount_point);
            scanner.set_recursive(true);
            scanner.set_max_files(5000);
            scanner.set_default_tier(tier);
            auto result = scanner.scan(*catalog, false);
            total_added += static_cast<int>(result.added);
            if (result.added > 0)
                LOG_INFO("Main", "Added " + std::to_string(result.added) +
                         " files from " + di.mount_point);
        }
        if (total_added > 0)
            LOG_INFO("Main", "Startup scan complete: " + std::to_string(total_added) + " files added from all drives");
    }

    // ── Command-line scan ─────────────────────────────────
    if (!auto_scan_dir.empty()) {
        LOG_INFO("Main", "Scanning directory: " + auto_scan_dir);
        RealScanner scanner;
        scanner.set_root(auto_scan_dir);
        scanner.set_recursive(true);
        scanner.set_max_files(5000);
        auto result = scanner.scan(*catalog, true);
        LOG_INFO("Main", "Scan complete: " + std::to_string(result.added) + " files added");
    }

    // ── File Watcher ─────────────────────────────────────────
    std::unique_ptr<FileWatcher> watcher;
    if (cfg.enable_file_watcher && !cfg.scan_root.empty()) {
        watcher = std::make_unique<FileWatcher>();
        watcher->watch(cfg.scan_root, cfg.recursive_scan);
        watcher->on_created([&](const std::string& path) {
            LOG_DEBUG("Watcher", "New file: " + path);
            try {
                auto existing = catalog->get_file_by_path(path);
                if (!existing.has_value()) {
                    fs::path file_p(path);
                    if (fs::exists(file_p) && fs::is_regular_file(file_p)) {
                        std::string ext = file_p.extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        std::string file_id = "watch_" + std::to_string(std::time(nullptr))
                                             + "_" + std::to_string(std::hash<std::string>{}(path) % 100000);
                        FileRecord f;
                        f.id            = file_id;
                        f.path          = path;
                        f.extension     = ext;
                        f.file_type     = classify_extension(ext);
                        f.size_bytes    = static_cast<int64_t>(fs::file_size(file_p));
                        f.access_count  = 0;
                        f.write_count   = 0;
                        f.created_at    = std::time(nullptr);
                        f.last_accessed = std::time(nullptr);
                        f.last_modified = std::time(nullptr);
                        f.current_tier  = Tier::HOT;
                        f.target_tier   = Tier::HOT;
                        f.migrate_count = 0;
                        f.is_pinned     = false;
                        f.is_critical   = false;
                        f.score         = 0.0;
                        catalog->add_file(f);
                        LOG_INFO("Watcher", "Auto-registered new file: " + path);
                    }
                }
            } catch (...) {}
        });
        watcher->on_modified([&](const std::string& path) {
            // Update file size/timestamp
            auto file_opt = catalog->get_file_by_path(path);
            if (file_opt.has_value()) {
                auto f = file_opt.value();
                try {
                    f.size_bytes = static_cast<int64_t>(std::filesystem::file_size(path));
                    f.last_modified = time(nullptr);
                    catalog->update_file(f);
                } catch (...) {}
            }
        });
        watcher->start();
        LOG_INFO("Main", "File watcher started on " + cfg.scan_root);
    }

    // ── Access Tracker ───────────────────────────────────────
    std::unique_ptr<AccessTracker> tracker;
    if (cfg.enable_access_tracker && !cfg.scan_root.empty()) {
        tracker = std::make_unique<AccessTracker>();
        tracker->set_catalog(catalog.get());
        tracker->start(cfg.scan_root);
        LOG_INFO("Main", "Access tracker started");
    }

    // ── Supabase ─────────────────────────────────────────────
    auto supabase = std::make_unique<SupabaseClient>();
    SupabaseConfig sc;
    sc.project_url  = cfg.supabase_url;
    sc.anon_key     = cfg.supabase_anon_key;
    sc.enabled      = cfg.supabase_enabled;
    supabase->configure(sc);
    if (sc.enabled)
        LOG_INFO("Main", "Supabase client configured: " + cfg.supabase_url);

    // ── REST API ─────────────────────────────────────────────
    std::unique_ptr<RestServer> api;
    if (cfg.enable_rest_api) {
        std::string api_key = cfg.api_key;
        const char* env_key = std::getenv("OPENCODE_API_KEY");
        if (env_key && *env_key)
            api_key = env_key;
        api = std::make_unique<RestServer>(catalog, policy, engine);
        api->auth().set_supabase(supabase.get());
        api->start(cfg.rest_api_port, api_key, cfg.jwt_secret);
        std::cout << GREEN << "  REST API: http://localhost:"
                  << cfg.rest_api_port << "/\n" << RESET;

        // Create default admin user if no users exist
        api->auth().ensure_default_admin("users.json");
        if (cfg.supabase_enabled)
            std::cout << GREEN << "  Auth: Supabase enabled (admin/admin123)\n" << RESET;
        else
            std::cout << GREEN << "  Auth: default admin user created (admin/admin123)\n" << RESET;
    }

    // ── S3 API ───────────────────────────────────────────────
    std::unique_ptr<S3Server> s3_api;
    if (cfg.enable_s3_api) {
        s3_api = std::make_unique<S3Server>(catalog, cfg.s3_base_path, cfg.api_key);
        s3_api->start(cfg.s3_api_port);
        std::cout << GREEN << "  S3 API:  http://localhost:"
                  << cfg.s3_api_port << "/\n" << RESET;
    }

    // ── Daemon mode ──────────────────────────────────────────
    if (cfg.run_as_daemon) {
        std::cout << "  Running in daemon mode (Ctrl+C to stop)\n";
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        run_daemon_loop(engine, catalog, policy);
        return 0;
    }

    // ── Run once mode ────────────────────────────────────────
    if (run_once) {
        auto result = engine->run_cycle();
        std::cout << "  Cycle: " << result.migrated << " migrated, "
                  << result.failed << " failed\n";
        return 0;
    }

    // ── Interactive terminal mode ────────────────────────────
    Dashboard dash(catalog, policy, engine);
    dash.clear();
    dash.print_banner();

    if (catalog->file_count() > 0)
        dash.print_tier_summary();

    std::cout << "\n  Press Enter to continue...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();

    while (true) {
        dash.clear();
        dash.print_banner();
        dash.print_tier_summary();
        dash.print_engine_state();

        std::cout << "\n" << BOLD << CYAN
            << "  [1]Run Cycle  [2]Summary  [3]History  [4]Analyse\n"
            << "  [5]HOT List   [6]WARM List [7]COLD List [8]ARCH List\n"
            << "  [f]Access File  [sc]Scan Real Folder\n"
            << "  [a]Add File  [p]Pin/Unpin  [s]Savings  [st]Status  [h]Help  [q]Quit\n"
            << RESET;
        std::cout << "  > ";

        std::string choice;
        if (!(std::cin >> choice)) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }

        if (choice == "q" || choice == "Q") {
            std::cout << GREEN << "  Goodbye!\n" << RESET;
            break;
        } else if (choice == "1") {
            dash.clear(); dash.print_banner();
            auto result = engine->run_cycle();
            if (result.migrated > 0) {
                std::cout << "\n  " << GREEN << result.migrated
                          << " migration(s) completed.\n" << RESET;
            } else {
                std::cout << "\n  " << GREEN << "All files on optimal tiers.\n" << RESET;
            }
            if (result.failed > 0)
                std::cout << "  " << RED << result.failed << " failed.\n" << RESET;
            dash.print_tier_summary();
        } else if (choice == "2") {
            dash.clear(); dash.print_banner(); dash.print_tier_summary();
        } else if (choice == "3") {
            dash.clear(); dash.print_banner(); dash.print_recent_migrations(15);
        } else if (choice == "4") {
            dash.clear(); dash.print_banner(); dash.print_analysis();
        } else if (choice == "5") { dash.clear(); dash.print_banner(); dash.print_file_list(Tier::HOT);
        } else if (choice == "6") { dash.clear(); dash.print_banner(); dash.print_file_list(Tier::WARM);
        } else if (choice == "7") { dash.clear(); dash.print_banner(); dash.print_file_list(Tier::COLD);
        } else if (choice == "8") { dash.clear(); dash.print_banner(); dash.print_file_list(Tier::ARCHIVE);
        } else if (choice == "f" || choice == "F") {
            dash.clear(); dash.print_banner();
            dash.access_file_interactive();
        } else if (choice == "sc" || choice == "SC") {
            dash.clear(); dash.print_banner();
            run_interactive_scan(*catalog);
        } else if (choice == "a") {
            dash.add_file_interactive();
        } else if (choice == "p") {
            dash.pin_file_interactive();
        } else if (choice == "s") {
            dash.clear(); dash.print_banner(); dash.print_savings_report();
        } else if (choice == "st") {
            dash.clear(); dash.print_banner(); dash.print_system_status();
        } else if (choice == "h") {
            dash.clear(); dash.print_help();
        } else {
            std::cout << "  " << RED << "Unknown command. [h] for help.\n" << RESET;
        }

        std::cout << "\n  " << DIM << "Press Enter..." << RESET;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        if (std::cin.peek() == '\n') std::cin.ignore();
    }

    // ── Shutdown ─────────────────────────────────────────────
    LOG_INFO("Main", "Shutting down");
    if (drive_detector) drive_detector->stop();
    if (watcher) watcher->stop();
    if (tracker) tracker->stop();
    if (s3_api) s3_api->stop();
    if (api) api->stop();
    catalog->close();

    return 0;
}
