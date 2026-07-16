#include "api/rest_server.hpp"
#include "core/types.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "monitoring/drive_detector.hpp"
#include "scanner/real_scanner.hpp"
#include <httplib.h>
#include <sstream>
#include <fstream>
#include <optional>
#include <thread>
#include <algorithm>
#include <cctype>

RestServer::RestServer(std::shared_ptr<CatalogInterface> catalog,
                       std::shared_ptr<PolicyEngine> policy,
                       std::shared_ptr<MigrationEngine> engine)
    : catalog_(std::move(catalog))
    , policy_(std::move(policy))
    , engine_(std::move(engine))
{
    auto& cfg = ConfigManager::instance().config();
    file_server_ = std::make_unique<FileServer>(cfg.scan_root);
}

bool RestServer::start(int port, const std::string& api_key,
                        const std::string& jwt_secret) {
    if (running_.load()) return false;
    port_ = port;
    if (!jwt_secret.empty()) auth_.set_secret(jwt_secret);

    running_.store(true);
    server_thread_ = std::thread([this, port, api_key]() {
        server_loop(port, api_key);
    });
    server_thread_.detach();

    LOG_INFO("REST API", "Server starting on port " + std::to_string(port));
    return true;
}

void RestServer::stop() {
    running_.store(false);
}

void RestServer::server_loop(int port, const std::string& api_key) {
    httplib::Server svr;

    // CORS — allow all origins (Electron loads from file://, browser from localhost)
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization, X-API-Key"}
    });

    // Handle CORS preflight for all routes
    svr.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // Auth middleware — returns AuthContext if authenticated
    auto authenticate = [&](const httplib::Request& req, httplib::Response& res) -> std::optional<AuthContext> {
        AuthContext ctx;

        // Check X-API-Key header first
        auto api_key_header = req.get_header_value("X-API-Key");
        if (!api_key.empty() && !api_key_header.empty() && api_key_header == api_key) {
            ctx.authenticated = true;
            ctx.is_admin = true;
            ctx.role = UserRole::ADMIN;
            ctx.username = "admin";
            return ctx;
        }

        auto auth_header = req.get_header_value("Authorization");

        if (!auth_header.empty()) {
            if (auth_header.size() > 7 && auth_header.substr(0, 7) == "Bearer ") {
                std::string token = auth_header.substr(7);
                // Check if the token matches the API key directly
                if (!api_key.empty() && token == api_key) {
                    ctx.authenticated = true;
                    ctx.is_admin = true;
                    ctx.role = UserRole::ADMIN;
                    ctx.username = "admin";
                    return ctx;
                }
                // Otherwise verify as JWT
                auto payload = auth_.verify_token(token);
                if (payload) {
                    ctx.user_id = payload->user_id;
                    ctx.username = payload->username;
                    ctx.role = payload->role;
                    ctx.is_admin = (payload->role == UserRole::ADMIN);
                    ctx.authenticated = true;
                    return ctx;
                }
            }
        }

        if (!api_key.empty()) {
            res.status = 401;
            res.set_content(error_response(401, "Unauthorized"), "application/json");
            return std::nullopt;
        }

        ctx.authenticated = false;
        return ctx;
    };

    // ── Auth Routes (no auth required) ──────────────────────

    svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    svr.Post("/api/v1/auth/register", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        if (!ctx->is_admin) {
            res.status = 403;
            res.set_content(error_response(403, "Admin access required"), "application/json");
            return;
        }
        res.set_content(handle_auth_register(req.body), "application/json");
    });

    svr.Post("/api/v1/auth/login", [&](const httplib::Request& req, httplib::Response& res) {
        res.set_content(handle_auth_login(req.body), "application/json");
    });

    // ── Protected Routes ────────────────────────────────────

    // Dashboard is served without auth (it passes credentials via API calls)
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        if (!catalog_) { res.status = 500; return; }
        res.set_content(html_dashboard(api_key), "text/html");
    });

    svr.Get("/api/v1/auth/me", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        res.set_content(handle_auth_me(*ctx), "application/json");
    });

    svr.Get("/api/v1/auth/users", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        if (!ctx->is_admin) { res.status = 403; res.set_content(error_response(403, "Forbidden"), "application/json"); return; }
        res.set_content(handle_auth_users(), "application/json");
    });

    svr.Delete(R"(/api/v1/auth/users/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        if (!ctx->is_admin) { res.status = 403; res.set_content(error_response(403, "Forbidden"), "application/json"); return; }
        res.set_content(handle_auth_delete_user(req.matches[1]), "application/json");
    });

    svr.Get("/api/v1/files", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        std::string query;
        for (const auto& [k, v] : req.params)
            query += (query.empty() ? "" : "&") + k + "=" + v;
        res.set_content(handle_list_files(query, *ctx), "application/json");
    });

    svr.Get("/api/v1/files/(.+)", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        res.set_content(handle_get_file(req.matches[1], *ctx), "application/json");
    });

    svr.Post("/api/v1/files/(.+)/touch", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        std::string file_id = req.matches[1];
        if (file_id.empty() || file_id.size() > 64 ||
            !std::all_of(file_id.begin(), file_id.end(),
                [](char c) { return std::isalnum(c) || c == '_' || c == '-'; })) {
            res.status = 400;
            res.set_content(error_response(400, "Invalid file ID"), "application/json");
            return;
        }
        res.set_content(handle_touch_file(file_id, *ctx), "application/json");
    });

    svr.Post("/api/v1/files/(.+)/pin", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        std::string file_id = req.matches[1];
        if (file_id.empty() || file_id.size() > 64 ||
            !std::all_of(file_id.begin(), file_id.end(),
                [](char c) { return std::isalnum(c) || c == '_' || c == '-'; })) {
            res.status = 400;
            res.set_content(error_response(400, "Invalid file ID"), "application/json");
            return;
        }
        bool pin = true;
        try {
            auto j = nlohmann::json::parse(req.body);
            if (j.contains("pin")) pin = j["pin"].get<bool>();
        } catch (...) {}
        res.set_content(handle_pin_file(file_id, pin, *ctx), "application/json");
    });

    svr.Get("/api/v1/tiers", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        res.set_content(handle_get_tiers(), "application/json");
    });

    svr.Get("/api/v1/drives", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        res.set_content(handle_get_drives(), "application/json");
    });

    svr.Get("/api/v1/tiers/(\\d+)", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        int tier_idx = 0;
        try { tier_idx = std::stoi(req.matches[1]); }
        catch (...) { res.status = 400; res.set_content(error_response(400, "Invalid tier index"), "application/json"); return; }
        res.set_content(handle_list_by_tier(tier_idx), "application/json");
    });

    svr.Post("/api/v1/cycle", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        res.set_content(handle_run_cycle(), "application/json");
    });

    svr.Get("/api/v1/cycle/history", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        int n = 20;
        if (req.has_param("n")) {
            try { n = std::stoi(req.get_param_value("n")); }
            catch (...) { n = 20; }
        }
        res.set_content(handle_get_history(n), "application/json");
    });

    svr.Get("/api/v1/analysis", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        res.set_content(handle_get_analysis(), "application/json");
    });

    svr.Get("/api/v1/savings", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        res.set_content(handle_get_savings(), "application/json");
    });

    svr.Get("/api/v1/metrics", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        res.set_content(handle_get_metrics(), "text/plain");
    });

    svr.Post("/api/v1/config/reload", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        res.set_content(handle_reload_config(), "application/json");
    });

    svr.Get("/api/v1/engine", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        nlohmann::json j;
        j["state"] = engine_->state_name();
        j["cycle_count"] = engine_->cycle_count();
        j["total_migrations"] = engine_->total_migrations();
        j["total_bytes_migrated"] = engine_->total_bytes_migrated();
        if (catalog_) {
            j["file_count"] = catalog_->file_count();
            j["total_bytes"] = catalog_->total_bytes();
        }
        res.set_content(json_response(200, j.dump()), "application/json");
    });

    // ── Scan endpoint ────────────────────────────────────────
    svr.Post("/api/v1/scan", [&](const httplib::Request& req, httplib::Response& res) {
        auto ctx = authenticate(req, res);
        if (!ctx) return;
        try {
            bool scan_all = false;
            std::string scan_root = ConfigManager::instance().config().scan_root;
            // Allow overriding scan_root or scanning all drives from request body
            if (!req.body.empty()) {
                auto j = nlohmann::json::parse(req.body);
                if (j.contains("path")) scan_root = j["path"].get<std::string>();
                if (j.contains("scan_all")) scan_all = j["scan_all"].get<bool>();
            }

            int total_added = 0;
            int total_scanned = 0;
            int total_skipped = 0;
            int total_errors = 0;

            if (scan_all) {
                // Scan all detected drives with proper tier assignment
                auto drives = DriveDetector::enumerate_drives();
                for (const auto& di : drives) {
                    Tier tier = drive_hardware_to_tier(di.hardware_type);
                    RealScanner scanner;
                    scanner.set_root(di.mount_point);
                    scanner.set_recursive(true);
                    scanner.set_max_files(10000);
                    scanner.set_default_tier(tier);
                    scanner.set_owner_id(ctx->user_id);
                    auto result = scanner.scan(*catalog_, false);
                    total_added += static_cast<int>(result.added);
                    total_scanned += static_cast<int>(result.scanned);
                    total_skipped += static_cast<int>(result.skipped);
                    total_errors += static_cast<int>(result.errors);
                }
            } else {
                if (scan_root.empty()) scan_root = ".";
                RealScanner scanner;
                scanner.set_root(scan_root);
                scanner.set_recursive(ConfigManager::instance().config().recursive_scan);
                scanner.set_max_files(10000);
                scanner.set_owner_id(ctx->user_id);
                auto result = scanner.scan(*catalog_, false);
                total_added = static_cast<int>(result.added);
                total_scanned = static_cast<int>(result.scanned);
                total_skipped = static_cast<int>(result.skipped);
                total_errors = static_cast<int>(result.errors);
            }

            nlohmann::json j;
            j["status"] = "ok";
            j["scanned"] = total_scanned;
            j["added"] = total_added;
            j["skipped"] = total_skipped;
            j["errors"] = total_errors;
            j["scan_all"] = scan_all;
            res.set_content(json_response(200, j.dump()), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(error_response(500, std::string("Scan failed: ") + e.what()), "application/json");
        }
    });

    // ── File Server Routes ───────────────────────────────────
    if (file_server_) {
        // Serve the file server HTML (no auth — credentials handled by JS)
        svr.Get("/fileserver", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(FileServer::html_content(), "text/html");
        });

        // File system list
        svr.Get("/api/v1/fs/list", [&](const httplib::Request& req, httplib::Response& res) {
            auto ctx = authenticate(req, res);
            if (!ctx) return;
            file_server_->set_user(ctx->username, ctx->is_admin);
            std::string path = req.get_param_value("path", false);
            std::string out_path;
            std::string json = file_server_->handle_list(path, out_path);
            res.set_content(json, "application/json");
        });

        // File upload
        svr.Post("/api/v1/fs/upload", [&](const httplib::Request& req, httplib::Response& res) {
            auto ctx = authenticate(req, res);
            if (!ctx) return;
            file_server_->set_user(ctx->username, ctx->is_admin);
            std::string path = req.get_param_value("path", false);
            bool ok = file_server_->handle_upload(path, req.body);
            if (!ok) {
                res.status = 500;
                res.set_content(R"({"error":"Upload failed"})", "application/json");
                return;
            }

            // Register uploaded file in catalog so tiering can track it
            if (catalog_) {
                std::string real_path;
                if (file_server_->resolve(path, real_path)) {
                    fs::path file_p(real_path);
                    if (fs::exists(file_p) && fs::is_regular_file(file_p)) {
                        auto existing = catalog_->get_file_by_path(real_path);
                        if (!existing.has_value()) {
                            std::string file_id = "upload_" + std::to_string(std::time(nullptr))
                                                 + "_" + std::to_string(std::hash<std::string>{}(real_path) % 100000);
                            FileRecord f;
                            f.id            = file_id;
                            f.path          = real_path;
                            f.extension     = file_p.extension().string();
                            std::transform(f.extension.begin(), f.extension.end(),
                                           f.extension.begin(), ::tolower);
                            f.file_type     = classify_extension(f.extension);
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
                            f.owner_id      = ctx->user_id;
                            catalog_->add_file(f);
                        }
                    }
                }
            }

            res.set_content(R"({"status":"ok"})", "application/json");
        });

        // File download
        svr.Get("/api/v1/fs/download", [&](const httplib::Request& req, httplib::Response& res) {
            auto ctx = authenticate(req, res);
            if (!ctx) return;
            file_server_->set_user(ctx->username, ctx->is_admin);
            std::string path = req.get_param_value("path", false);
            std::string data, mime;
            if (file_server_->handle_download(path, data, mime)) {
                res.set_content(data, mime);
            } else {
                res.status = 404;
                res.set_content(R"({"error":"File not found"})", "application/json");
            }
        });

        // File delete
        svr.Delete("/api/v1/fs/delete", [&](const httplib::Request& req, httplib::Response& res) {
            auto ctx = authenticate(req, res);
            if (!ctx) return;
            file_server_->set_user(ctx->username, ctx->is_admin);
            std::string path = req.get_param_value("path", false);
            bool ok = file_server_->handle_delete(path);
            if (ok)
                res.set_content(R"({"status":"ok"})", "application/json");
            else {
                res.status = 404;
                res.set_content(R"({"error":"Delete failed"})", "application/json");
            }
        });

        // Create directory
        svr.Post("/api/v1/fs/mkdir", [&](const httplib::Request& req, httplib::Response& res) {
            auto ctx = authenticate(req, res);
            if (!ctx) return;
            file_server_->set_user(ctx->username, ctx->is_admin);
            std::string path = req.get_param_value("path", false);
            bool ok = file_server_->handle_mkdir(path);
            if (ok)
                res.set_content(R"({"status":"ok"})", "application/json");
            else {
                res.status = 500;
                res.set_content(R"({"error":"mkdir failed"})", "application/json");
            }
        });

        // Rename
        svr.Post("/api/v1/fs/rename", [&](const httplib::Request& req, httplib::Response& res) {
            auto ctx = authenticate(req, res);
            if (!ctx) return;
            file_server_->set_user(ctx->username, ctx->is_admin);
            try {
                auto j = nlohmann::json::parse(req.body);
                std::string old_path = j.value("old_path", "");
                std::string new_path = j.value("new_path", "");
                bool ok = file_server_->handle_rename(old_path, new_path);
                if (ok)
                    res.set_content(R"({"status":"ok"})", "application/json");
                else {
                    res.status = 500;
                    res.set_content(R"({"error":"Rename failed"})", "application/json");
                }
            } catch (...) {
                res.status = 400;
                res.set_content(R"({"error":"Invalid request"})", "application/json");
            }
        });

        // Server stats
        svr.Get("/api/v1/stats", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(file_server_->handle_stats(), "application/json");
        });
    }

    LOG_INFO("REST API", "Listening on http://127.0.0.1:" + std::to_string(port));
    svr.listen("127.0.0.1", port);
}

// ── Route Handlers ──────────────────────────────────────────

std::string RestServer::handle_list_files(const std::string& query, const AuthContext& ctx) {
    if (!catalog_) return error_response(500, "Catalog not initialized");

    std::vector<FileRecord> files;
    if (ctx.is_admin) {
        // Admin users (API key or admin JWT) see all files
        files = catalog_->all_files();
    } else if (!ctx.user_id.empty()) {
        // Regular users see only their own files
        files = catalog_->files_by_owner(ctx.user_id);
    } else {
        files = catalog_->all_files();
    }

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& f : files)
        arr.push_back(nlohmann::json::parse(file_record_to_json(f)));

    nlohmann::json resp;
    resp["count"] = files.size();
    resp["files"] = arr;
    return json_response(200, resp.dump());
}

std::string RestServer::handle_get_file(const std::string& id, const AuthContext& ctx) {
    if (!catalog_) return error_response(500, "Catalog not initialized");
    auto file = catalog_->get_file(id);
    if (!file.has_value())
        return error_response(404, "File not found");

    // Check ownership: non-admin users can only access their own files
    if (!ctx.is_admin && !ctx.user_id.empty() && file->owner_id != ctx.user_id)
        return error_response(403, "Access denied");

    return json_response(200, file_record_to_json(file.value()));
}

std::string RestServer::handle_touch_file(const std::string& id, const AuthContext& ctx) {
    if (!catalog_) return error_response(500, "Catalog not initialized");

    // Check ownership
    if (!ctx.is_admin && !ctx.user_id.empty()) {
        auto file = catalog_->get_file(id);
        if (!file.has_value() || file->owner_id != ctx.user_id)
            return error_response(404, "File not found");
    }

    bool ok = catalog_->record_access(id);
    if (!ok) return error_response(404, "File not found");

    nlohmann::json j;
    j["status"] = "ok";
    j["file_id"] = id;
    return json_response(200, j.dump());
}

std::string RestServer::handle_pin_file(const std::string& id, bool pin, const AuthContext& ctx) {
    if (!catalog_) return error_response(500, "Catalog not initialized");
    auto file_opt = catalog_->get_file(id);
    if (!file_opt.has_value()) return error_response(404, "File not found");

    auto f = file_opt.value();
    f.is_pinned = pin;
    if (pin) f.current_tier = Tier::HOT;
    if (!catalog_->update_file(f)) return error_response(500, "Failed to update file");

    nlohmann::json j;
    j["status"] = "ok";
    j["file_id"] = id;
    j["is_pinned"] = pin;
    return json_response(200, j.dump());
}

std::string RestServer::handle_get_drives() {
    auto drives = DriveDetector::enumerate_drives();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& d : drives) {
        nlohmann::json j;
        j["mount_point"] = d.mount_point;
        j["label"]       = d.label;
        j["hardware_type"] = static_cast<int>(d.hardware_type);
        j["hardware_name"] = [](DriveHardwareType t) {
            switch (t) {
                case DriveHardwareType::SSD:    return "SSD";
                case DriveHardwareType::HDD:    return "HDD";
                case DriveHardwareType::USB:    return "USB";
                case DriveHardwareType::REMOTE: return "Remote";
                default:                        return "Unknown";
            }
        }(d.hardware_type);
        j["storage_class"] = TIER_NAMES[static_cast<int>(drive_hardware_to_tier(d.hardware_type))];
        j["total_bytes"]  = d.total_size_bytes;
        j["free_bytes"]   = d.free_size_bytes;
        j["used_bytes"]   = d.total_size_bytes - d.free_size_bytes;
        j["usage_pct"]    = d.total_size_bytes > 0
            ? (100.0 * (d.total_size_bytes - d.free_size_bytes) / d.total_size_bytes) : 0.0;
        arr.push_back(j);
    }
    nlohmann::json resp;
    resp["drives"] = arr;
    return json_response(200, resp.dump());
}

std::string RestServer::handle_get_tiers() {
    if (!catalog_) return error_response(500, "Catalog not initialized");
    auto stats = catalog_->all_tier_stats();

    nlohmann::json arr = nlohmann::json::array();
    for (int i = 0; i < 4; ++i) {
        nlohmann::json j;
        j["tier"]        = TIER_NAMES[i];
        j["file_count"]  = stats[i].file_count;
        j["total_bytes"] = stats[i].total_bytes;
        j["monthly_cost"] = stats[i].monthly_cost_usd;
        j["accesses"]    = stats[i].total_accesses;
        arr.push_back(j);
    }

    nlohmann::json resp;
    resp["tiers"] = arr;
    return json_response(200, resp.dump());
}

std::string RestServer::handle_list_by_tier(int tier_idx) {
    if (!catalog_) return error_response(500, "Catalog not initialized");
    if (tier_idx < 0 || tier_idx >= 4)
        return error_response(400, "Invalid tier index");

    auto files = catalog_->files_by_tier(static_cast<Tier>(tier_idx));
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& f : files)
        arr.push_back(nlohmann::json::parse(file_record_to_json(f)));

    nlohmann::json resp;
    resp["tier"] = TIER_NAMES[tier_idx];
    resp["count"] = files.size();
    resp["files"] = arr;
    return json_response(200, resp.dump());
}

std::string RestServer::handle_run_cycle() {
    if (!engine_) return error_response(500, "Engine not initialized");

    if (engine_->is_running())
        return error_response(409, "Migration cycle already running");

    auto result = engine_->run_cycle();
    nlohmann::json j;
    j["status"] = "completed";
    j["recommended"] = result.total_recommended;
    j["migrated"]    = result.migrated;
    j["failed"]      = result.failed;
    j["bytes_migrated"] = result.bytes_migrated;
    j["duration_ms"] = result.duration_ms;
    j["monthly_savings"] = result.estimated_monthly_savings;
    return json_response(200, j.dump());
}

std::string RestServer::handle_get_history(int n) {
    if (!engine_) return error_response(500, "Engine not initialized");
    auto events = engine_->recent_history(n);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : events)
        arr.push_back(nlohmann::json::parse(migration_event_to_json(e)));

    nlohmann::json resp;
    resp["count"] = events.size();
    resp["migrations"] = arr;
    return json_response(200, resp.dump());
}

std::string RestServer::handle_get_analysis() {
    if (!policy_ || !catalog_)
        return error_response(500, "Policy or catalog not initialized");

    auto recs = policy_->analyse(*catalog_);
    nlohmann::json arr = nlohmann::json::array();
    double total_savings = 0;

    for (const auto& r : recs) {
        nlohmann::json j;
        j["file_id"]      = r.file.id;
        j["file_path"]    = r.file.path;
        j["current_tier"] = TIER_NAMES[static_cast<int>(r.file.current_tier)];
        j["target_tier"]  = TIER_NAMES[static_cast<int>(r.target_tier)];
        j["score"]        = r.score;
        j["reason"]       = r.reason;
        j["monthly_savings"] = r.estimated_monthly_savings;
        total_savings += r.estimated_monthly_savings;
        arr.push_back(j);
    }

    nlohmann::json resp;
    resp["recommendations"] = arr.size();
    resp["total_monthly_savings"] = total_savings;
    resp["results"] = arr;
    return json_response(200, resp.dump());
}

std::string RestServer::handle_get_savings() {
    if (!catalog_ || !engine_)
        return error_response(500, "Engine not initialized");

    nlohmann::json j;
    j["current_monthly_cost"] = catalog_->total_monthly_cost();
    j["savings_vs_all_hot"]   = engine_->savings_vs_all_hot(*catalog_);
    j["savings_percent"]      = engine_->savings_percent(*catalog_);
    j["total_migrations"]     = engine_->total_migrations();
    j["total_bytes_migrated"] = engine_->total_bytes_migrated();

    // Projected annual
    double savings = engine_->savings_vs_all_hot(*catalog_);
    j["projected_annual_savings"] = savings * 12.0;

    return json_response(200, j.dump());
}

std::string RestServer::handle_get_metrics() {
    if (!catalog_ || !engine_)
        return error_response(500, "Engine not initialized");

    std::ostringstream oss;
    auto stats = catalog_->all_tier_stats();

    oss << "# HELP storage_tiering_files_total Total files managed\n";
    oss << "# TYPE storage_tiering_files_total gauge\n";
    for (int i = 0; i < 4; ++i) {
        oss << "storage_tiering_files_total{tier=\"" << TIER_NAMES[i]
            << "\"} " << stats[i].file_count << "\n";
    }

    oss << "# HELP storage_tiering_bytes_total Total bytes per tier\n";
    oss << "# TYPE storage_tiering_bytes_total gauge\n";
    for (int i = 0; i < 4; ++i) {
        oss << "storage_tiering_bytes_total{tier=\"" << TIER_NAMES[i]
            << "\"} " << stats[i].total_bytes << "\n";
    }

    oss << "# HELP storage_tiering_monthly_cost Monthly storage cost by tier\n";
    oss << "# TYPE storage_tiering_monthly_cost gauge\n";
    for (int i = 0; i < 4; ++i) {
        oss << "storage_tiering_monthly_cost{tier=\"" << TIER_NAMES[i]
            << "\"} " << stats[i].monthly_cost_usd << "\n";
    }

    oss << "# HELP storage_tiering_migrations_total Total migrations performed\n";
    oss << "# TYPE storage_tiering_migrations_total counter\n";
    oss << "storage_tiering_migrations_total " << engine_->total_migrations() << "\n";

    oss << "# HELP storage_tiering_bytes_migrated_total Total bytes migrated\n";
    oss << "# TYPE storage_tiering_bytes_migrated_total counter\n";
    oss << "storage_tiering_bytes_migrated_total " << engine_->total_bytes_migrated() << "\n";

    return oss.str();
}

std::string RestServer::handle_reload_config() {
    bool ok = ConfigManager::instance().reload();
    if (ok) {
        nlohmann::json j;
        j["status"] = "ok";
        return json_response(200, j.dump());
    }
    return error_response(500, "Failed to reload config");
}

// ── Helpers ─────────────────────────────────────────────────

std::string RestServer::json_response(int status, const std::string& body) {
    nlohmann::json j;
    j["status"] = status;
    // body is already JSON; merge it
    try {
        auto data = nlohmann::json::parse(body);
        for (auto& [key, val] : data.items())
            j[key] = val;
    } catch (...) {
        j["data"] = body;
    }
    return j.dump();
}

std::string RestServer::error_response(int status, const std::string& message) {
    nlohmann::json body;
    body["error"] = message;
    return json_response(status, body.dump());
}

std::string RestServer::file_record_to_json(const FileRecord& f) {
    nlohmann::json j;
    j["id"]              = f.id;
    j["path"]            = f.path;
    j["extension"]       = f.extension;
    j["file_type"]       = file_type_name(f.file_type);
    j["current_tier"]    = TIER_NAMES[static_cast<int>(f.current_tier)];
    j["target_tier"]     = TIER_NAMES[static_cast<int>(f.target_tier)];
    j["size_bytes"]      = f.size_bytes;
    j["size_gb"]         = f.size_gb();
    j["access_count"]    = f.access_count;
    j["write_count"]     = f.write_count;
    j["created_at"]      = format_time(f.created_at);
    j["last_accessed"]   = format_time(f.last_accessed);
    j["last_modified"]   = format_time(f.last_modified);
    j["migrate_count"]   = f.migrate_count;
    j["is_pinned"]       = f.is_pinned;
    j["is_critical"]     = f.is_critical;
    j["score"]           = f.score;
    j["monthly_cost"]    = f.monthly_cost();
    j["idle_days"]       = f.idle_days();
    j["age_days"]        = f.age_days();
    return j.dump();
}

std::string RestServer::migration_event_to_json(const MigrationEvent& e) {
    nlohmann::json j;
    j["file_id"]     = e.file_id;
    j["file_path"]   = e.file_path;
    j["from_tier"]   = TIER_NAMES[static_cast<int>(e.from_tier)];
    j["to_tier"]     = TIER_NAMES[static_cast<int>(e.to_tier)];
    j["size_bytes"]  = e.size_bytes;
    j["reason"]      = e.reason;
    j["timestamp"]   = e.time_str();
    j["success"]     = e.success;
    j["duration_ms"] = e.duration_ms;
    return j.dump();
}

// ── Auth Handlers ──────────────────────────────────────────

std::string RestServer::handle_auth_register(const std::string& body) {
    try {
        auto j = nlohmann::json::parse(body);
        std::string username = j.value("username", "");
        std::string password = j.value("password", "");
        std::string display_name = j.value("display_name", "");
        std::string role_str = j.value("role", "user");
        UserRole role = user_role_from_string(role_str);
        auto result = auth_.register_user(username, password, display_name, role);
        if (!result.success) return error_response(400, result.error);
        auth_.save_users("users.json");
        auto token = auth_.create_token(result.user->id, result.user->username, result.user->role);
        nlohmann::json r;
        r["token"] = token;
        r["user"] = {{"id", result.user->id}, {"username", result.user->username}, {"display_name", result.user->display_name}, {"role", user_role_name(result.user->role)}};
        return json_response(201, r.dump());
    } catch (const std::exception& e) {
        return error_response(400, std::string("Invalid request: ") + e.what());
    }
}

std::string RestServer::handle_auth_login(const std::string& body) {
    try {
        auto j = nlohmann::json::parse(body);
        std::string username = j.value("username", "");
        std::string password = j.value("password", "");
        auto result = auth_.login_user(username, password);
        if (!result.success) return error_response(401, result.error);
        auto token = auth_.create_token(result.user->id, result.user->username, result.user->role);
        nlohmann::json r;
        r["token"] = token;
        r["user"] = {{"id", result.user->id}, {"username", result.user->username}, {"display_name", result.user->display_name}, {"role", user_role_name(result.user->role)}};
        return json_response(200, r.dump());
    } catch (const std::exception& e) {
        return error_response(400, std::string("Invalid request: ") + e.what());
    }
}

std::string RestServer::handle_auth_me(const AuthContext& ctx) {
    auto result = auth_.get_user(ctx.user_id);
    if (!result.success) return error_response(404, "User not found");
    nlohmann::json r;
    r["user"] = {{"id", result.user->id}, {"username", result.user->username}, {"display_name", result.user->display_name}, {"role", user_role_name(result.user->role)}, {"created_at", result.user->created_at}, {"last_login", result.user->last_login}};
    return json_response(200, r.dump());
}

std::string RestServer::handle_auth_users() {
    auto users = auth_.list_users();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& u : users)
        arr.push_back({{"id", u.id}, {"username", u.username}, {"display_name", u.display_name}, {"role", user_role_name(u.role)}, {"created_at", u.created_at}, {"last_login", u.last_login}});
    nlohmann::json r;
    r["users"] = arr;
    r["count"] = users.size();
    return json_response(200, r.dump());
}

std::string RestServer::handle_auth_delete_user(const std::string& user_id) {
    if (auth_.delete_user(user_id)) {
        auth_.save_users("users.json");
        return json_response(200, "{\"status\":\"deleted\"}");
    }
    return error_response(404, "User not found");
}

std::string RestServer::html_dashboard(const std::string& api_key) {
    const std::vector<std::string> search_paths = {
        "./dashboard.html", "../src/api/dashboard.html",
        "src/api/dashboard.html", "../dashboard.html"
    };
    for (const auto& path : search_paths) {
        std::ifstream ifs(path);
        if (ifs.is_open()) {
            std::stringstream buf;
            buf << ifs.rdbuf();
            auto html = buf.str();
            if (!html.empty()) {
                LOG_DEBUG("Dashboard", "Loaded from " + path);
                return html;
            }
        }
    }
    LOG_WARN("Dashboard", "dashboard.html not found, using fallback");
    std::string html = std::string(R"raw(
<!DOCTYPE html><html lang="en">
<head><meta charset="UTF-8">
<title>Storage Tiering Dashboard</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4"></script>
<script>
const API_KEY = '__API_KEY__';
</script>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
         background: #0f0f1a; color: #c8d6e5; padding: 20px; }
  h1 { color: #00d4ff; font-size: 22px; margin-bottom: 16px; display: flex; align-items: center; gap: 12px; }
  h2 { color: #b0c4de; font-size: 14px; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 12px; }
  .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 16px; flex-wrap: wrap; gap: 8px; }
  .status-dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; margin-right: 6px; }
  .status-dot.online { background: #2ecc71; }
  .status-dot.offline { background: #e74c3c; }

  /* Tabs */
  .tabs { display: flex; gap: 4px; margin-bottom: 16px; }
  .tab { padding: 8px 18px; border-radius: 6px 6px 0 0; font-size: 13px; font-weight: 600;
         cursor: pointer; background: #1a1a2e; color: #667; border: 1px solid #2a2a4a; border-bottom: none; }
  .tab.active { background: #1a1a2e; color: #00d4ff; border-color: #00d4ff44; }
  .tab:hover { color: #b0c4de; }
  .panel { display: none; }
  .panel.active { display: block; }

  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 12px; margin-bottom: 16px; }
  .card { background: #1a1a2e; border-radius: 10px; padding: 16px; border: 1px solid #2a2a4a; }
  .card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; }
  .tier-badge { display: inline-block; padding: 3px 10px; border-radius: 4px; font-size: 11px; font-weight: 700; letter-spacing: 0.5px; }
  .tier-hot { background: #e74c3c22; color: #e74c3c; border: 1px solid #e74c3c44; }
  .tier-warm { background: #f39c1222; color: #f39c12; border: 1px solid #f39c1244; }
  .tier-cold { background: #3498db22; color: #3498db; border: 1px solid #3498db44; }
  .tier-archive { background: #9b59b622; color: #9b59b6; border: 1px solid #9b59b644; }
  .stat-value { font-size: 24px; font-weight: 700; color: #fff; }
  .stat-label { font-size: 11px; color: #667; margin-top: 2px; }
  .bar-bg { height: 4px; background: #2a2a4a; border-radius: 2px; margin-top: 10px; overflow: hidden; }
  .bar-fill { height: 100%; border-radius: 2px; transition: width 0.5s ease; }
  table { width: 100%; border-collapse: collapse; font-size: 13px; }
  th { text-align: left; padding: 8px 6px; color: #667; font-weight: 600; font-size: 11px; text-transform: uppercase; letter-spacing: 0.5px; border-bottom: 1px solid #2a2a4a; }
  td { padding: 8px 6px; border-bottom: 1px solid #1a1a2e; color: #c8d6e5; }
  tr:hover td { background: #1f1f35; }
  .btn { padding: 8px 18px; border: none; border-radius: 6px; font-size: 13px; font-weight: 600; cursor: pointer; transition: all 0.2s; }
  .btn-primary { background: #00d4ff; color: #0f0f1a; }
  .btn-primary:hover { background: #00b8e6; }
  .btn-primary:disabled { opacity: 0.5; cursor: not-allowed; }
  .btn-sm { padding: 4px 10px; font-size: 11px; }
  .btn-outline { background: transparent; color: #00d4ff; border: 1px solid #00d4ff44; }
  .btn-outline:hover { background: #00d4ff11; }
  .green { color: #2ecc71; }
  .red { color: #e74c3c; }
  .dim { color: #667; }
  .flex { display: flex; align-items: center; gap: 8px; }
  .flex-between { display: flex; justify-content: space-between; align-items: center; }
  .gap { gap: 12px; }
  .mt { margin-top: 12px; }
  .mb { margin-bottom: 12px; }
  .chart-container { position: relative; height: 300px; width: 100%; }
  .drive-card { margin-bottom: 8px; }
  .drive-label { width: 100px; display: inline-block; }
  .drive-type-badge { display: inline-block; padding: 2px 8px; border-radius: 3px; font-size: 10px; font-weight: 700; }
  .drive-type-ssd { background: #e74c3c22; color: #e74c3c; border: 1px solid #e74c3c44; }
  .drive-type-hdd { background: #f39c1222; color: #f39c12; border: 1px solid #f39c1244; }
  .drive-type-usb { background: #3498db22; color: #3498db; border: 1px solid #3498db44; }
  .drive-type-remote { background: #9b59b622; color: #9b59b6; border: 1px solid #9b59b644; }
  .drive-type-unknown { background: #6672222; color: #667; border: 1px solid #667444; }
  ::-webkit-scrollbar { width: 6px; }
  ::-webkit-scrollbar-track { background: #0f0f1a; }
  ::-webkit-scrollbar-thumb { background: #2a2a4a; border-radius: 3px; }
  @media (max-width: 600px) {
    body { padding: 10px; }
    .grid { grid-template-columns: 1fr 1fr; }
    .stat-value { font-size: 18px; }
  }
</style>
</head>
<body>
<div class="header">
  <h1>
    <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#00d4ff" stroke-width="2">
      <rect x="2" y="2" width="20" height="8" rx="2"/>
      <rect x="2" y="14" width="20" height="8" rx="2"/>
      <path d="M6 6h12M6 18h12"/>
    </svg>
    Storage Tiering
    <span style="font-size:12px;font-weight:400;"><span class="status-dot online" id="statusDot"></span><span id="statusText">Connected</span></span>
  </h1>
  <div class="flex gap">
    <button class="btn btn-primary btn-sm" onclick="runCycle()" id="cycleBtn">Run Cycle</button>
    <button class="btn btn-outline btn-sm" onclick="refreshAll()">Refresh</button>
  </div>
</div>

<div class="tabs">
  <div class="tab active" onclick="switchTab('overview')" id="tab-overview">Overview</div>
  <div class="tab" onclick="switchTab('drives')" id="tab-drives">Drives</div>
</div>

<div class="panel active" id="panel-overview">
  <div class="grid" id="tierCards"></div>
  <div class="grid" style="grid-template-columns:1fr 1fr;">
    <div class="card"><h2>Cost &amp; Savings</h2>
      <div id="savingsContent">
        <div class="stat-value" id="monthlyCost">--</div>
        <div class="stat-label">Monthly cost</div>
        <div class="mt" id="savingsDetails"></div>
      </div>
    </div>
    <div class="card"><h2>System Status</h2>
      <div id="systemStatus">
        <div class="flex-between"><span class="dim">Engine</span><span id="engineState">--</span></div>
        <div class="flex-between mt"><span class="dim">Files managed</span><span id="fileCount">--</span></div>
        <div class="flex-between mt"><span class="dim">Total size</span><span id="totalSize">--</span></div>
        <div class="flex-between mt"><span class="dim">Cycles run</span><span id="cycleCount">--</span></div>
        <div class="flex-between mt"><span class="dim">Migrations</span><span id="migrationCount">--</span></div>
        <div class="flex-between mt"><span class="dim">Bytes migrated</span><span id="bytesMigrated">--</span></div>
      </div>
    </div>
  </div>
  <div class="card">
    <h2>Storage Drives</h2>
    <div id="overviewDriveList"></div>
    <div id="noOverviewDrives" class="dim" style="text-align:center;padding:20px;">No drives detected</div>
  </div>
  <div class="card mt">
    <h2>Recent Migrations</h2>
    <div style="overflow-x:auto;">
      <table><thead><tr><th>Time</th><th>File</th><th>From</th><th>To</th><th>Size</th><th>Status</th></tr></thead>
        <tbody id="migrationTable"></tbody>
      </table>
    </div>
    <div id="noMigrations" class="dim" style="text-align:center;padding:20px;">No migrations yet</div>
  </div>
</div>

<div class="panel" id="panel-drives">
  <div id="driveList"></div>
  <div id="noDrives" class="card dim" style="text-align:center;padding:40px;">No drives detected</div>
  <div class="card mt">
    <h2>Storage Usage by Drive</h2>
    <div class="chart-container">
      <canvas id="driveChart"></canvas>
    </div>
  </div>
</div>

<script>
const BASE = '';
let refreshTimer = null;
const REFRESH_MS = 5000;
let driveChartInstance = null;

function switchTab(name) {
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  document.getElementById('tab-' + name).classList.add('active');
  document.getElementById('panel-' + name).classList.add('active');
  if (name === 'drives') loadDrives();
}

async function api(path, method) {
  method = method || 'GET';
  const h = {};
  if (API_KEY) h['X-API-Key'] = API_KEY;
  try {
    const r = await fetch(BASE + path, { method, headers: h });
    if (!r.ok) throw Error(r.status);
    return await r.json();
  } catch(e) {
    document.getElementById('statusDot').className='status-dot offline';
    document.getElementById('statusText').textContent='Disconnected (' + (e.message||'err') + ')';
    return null;
  }
}
async function runCycle() {
  const btn = document.getElementById('cycleBtn');
  btn.disabled = true; btn.textContent = 'Running...';
  const r = await api('/api/v1/cycle', 'POST');
  btn.disabled = false; btn.textContent = 'Run Cycle';
  if (r) refreshAll();
}
function renderTiers(data) {
  const c = document.getElementById('tierCards');
  if (!data||!data.tiers){c.innerHTML='<div class="card dim">No data</div>';return;}
  const total = data.tiers.reduce((s,t)=>s+t.total_bytes,0)||1;
  c.innerHTML = data.tiers.map(t => {
    const cls = t.tier.toLowerCase(); const pct = (t.total_bytes/total*100).toFixed(1);
    const gb = (t.total_bytes/1e9).toFixed(1);
    return '<div class="card"><div class="card-header"><span class="tier-badge tier-'+cls+'">'+t.tier+'</span><span class="dim">'+t.file_count+' files</span></div><div class="stat-value">'+gb+' <span class="dim" style="font-size:14px;font-weight:400;">GB</span></div><div class="stat-label">$'+(t.monthly_cost||0).toFixed(2)+'/mo</div><div class="bar-bg"><div class="bar-fill" style="width:'+pct+'%;background:'+(cls==='hot'?'#e74c3c':cls==='warm'?'#f39c12':cls==='cold'?'#3498db':'#9b59b6')+'"></div></div><div class="dim" style="font-size:11px;margin-top:4px;text-align:right;">'+pct+'%</div></div>';
  }).join('');
}
function renderSavings(data) {
  if (!data||data.current_monthly_cost===undefined) return;
  document.getElementById('monthlyCost').textContent='$'+data.current_monthly_cost.toFixed(2);
  document.getElementById('savingsDetails').innerHTML='<div class="green" style="font-size:20px;font-weight:700;">Savings: $'+(data.savings_vs_all_hot||0).toFixed(2)+'/mo ('+(data.savings_percent||0).toFixed(1)+'%)</div><div class="dim" style="font-size:13px;margin-top:4px;">Projected annual: $'+(data.projected_annual_savings||0).toFixed(2)+'</div>';
}
function renderStatus(engine,tiers) {
  document.getElementById('statusDot').className='status-dot online'; document.getElementById('statusText').textContent='Connected';
  if(engine){document.getElementById('engineState').textContent=engine.state||'--';document.getElementById('cycleCount').textContent=engine.cycle_count||0;document.getElementById('migrationCount').textContent=engine.total_migrations||0;}
  if(tiers){var tf=tiers.tiers?tiers.tiers.reduce(function(s,t){return s+t.file_count},0):0;var tb=tiers.tiers?tiers.tiers.reduce(function(s,t){return s+t.total_bytes},0):0;document.getElementById('fileCount').textContent=tf;document.getElementById('totalSize').textContent=((tb||0)/(1<<30)).toFixed(1)+' GB';}
}
function renderMigrations(data) {
  var tb=document.getElementById('migrationTable'),em=document.getElementById('noMigrations');
  if(!data||!data.migrations||!data.migrations.length){tb.innerHTML='';em.style.display='block';return;}
  em.style.display='none';
  tb.innerHTML=data.migrations.slice(-10).reverse().map(function(m){return '<tr><td class="dim">'+(m.timestamp||'--')+'</td><td style="max-width:200px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">'+(m.file_path||m.file_id||'--')+'</td><td><span class="tier-badge tier-'+(m.from_tier||'').toLowerCase()+'">'+(m.from_tier||'--')+'</span></td><td><span class="tier-badge tier-'+(m.to_tier||'').toLowerCase()+'">'+(m.to_tier||'--')+'</span></td><td>'+((m.size_bytes||0)/(1<<20)).toFixed(1)+' MB</td><td class="'+(m.success?'green':'red')+'">'+(m.success?'OK':'FAIL')+'</td></tr>';}).join('');
}

// ── Drives ────────────────────────────────────────────
function renderOverviewDrives(data) {
  var div = document.getElementById('overviewDriveList');
  var empty = document.getElementById('noOverviewDrives');
  if (!data || !data.drives || !data.drives.length) {
    div.innerHTML = ''; empty.style.display = 'block';
    return;
  }
  empty.style.display = 'none';
  div.innerHTML = data.drives.map(d => {
    var hw = (d.hardware_name || '').toLowerCase();
    var pct = d.usage_pct.toFixed(1);
    var used = formatBytes(d.used_bytes);
    var total = formatBytes(d.total_bytes);
    var sc = (d.storage_class || d.tier || '?').toLowerCase();
    return '<div style="display:flex;align-items:center;gap:12px;padding:8px 0;border-bottom:1px solid var(--border);">'
      + '<span class="drive-type-badge drive-type-' + hw + '">' + d.hardware_name + '</span>'
      + '<span style="font-weight:600;">' + d.label + '</span>'
      + '<span class="dim">(' + d.mount_point + ')</span>'
      + '<span class="tier-badge tier-' + sc + '">' + (d.storage_class || d.tier) + '</span>'
      + '<span style="margin-left:auto;">' + used + ' / ' + total + ' (' + pct + '%)</span>'
      + '</div>';
  }).join('');
}
async function loadDrives() {
  try {
    const data = await api('/api/v1/drives');
    const div = document.getElementById('driveList');
    const empty = document.getElementById('noDrives');
    if (!data || !data.drives || !data.drives.length) {
      div.innerHTML = ''; empty.style.display = 'block';
      if (driveChartInstance) { driveChartInstance.destroy(); driveChartInstance = null; }
      return;
    }
    empty.style.display = 'none';
    div.innerHTML = data.drives.map(d => {
      const hw = (d.hardware_name || '').toLowerCase();
      const pct = d.usage_pct.toFixed(1);
      const used = formatBytes(d.used_bytes);
      const total = formatBytes(d.total_bytes);
      const sc = (d.storage_class || d.tier || '?').toLowerCase();
      return '<div class="card drive-card"><div class="flex-between"><div class="flex">'
        + '<span class="drive-type-badge drive-type-' + hw + '">' + d.hardware_name + '</span>'
        + '<span style="margin-left:8px;font-weight:600;">' + d.label + '</span>'
        + '<span class="dim" style="margin-left:4px;">(' + d.mount_point + ')</span>'
        + '</div><span class="tier-badge tier-' + sc + '">' + (d.storage_class || d.tier) + '</span></div>'
        + '<div class="flex-between mt"><span class="dim">' + used + ' / ' + total + '</span><span style="font-weight:600;">' + pct + '%</span></div>'
        + '<div class="bar-bg"><div class="bar-fill" style="width:' + pct + '%;background:'
        + (hw === 'ssd' ? '#e74c3c' : hw === 'hdd' ? '#f39c12' : hw === 'usb' ? '#3498db' : hw === 'remote' ? '#9b59b6' : '#667') + '"></div></div>'
        + '</div>';
    }).join('');

  // Bar chart
  const ctx = document.getElementById('driveChart').getContext('2d');
  if (driveChartInstance) driveChartInstance.destroy();
  driveChartInstance = new Chart(ctx, {
    type: 'bar',
    data: {
      labels: data.drives.map(d => d.label + ' (' + d.mount_point + ')'),
      datasets: [
        {
          label: 'Used',
          data: data.drives.map(d => d.used_bytes),
          backgroundColor: '#e74c3c',
          borderRadius: 4
        },
        {
          label: 'Free',
          data: data.drives.map(d => d.free_bytes),
          backgroundColor: '#2ecc71',
          borderRadius: 4
        }
      ]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      plugins: {
        legend: {
          labels: { color: '#c8d6e5', font: { size: 12 } }
        }
      },
      scales: {
        x: {
          ticks: { color: '#667', font: { size: 11 } },
          grid: { color: '#2a2a4a' }
        },
        y: {
          beginAtZero: true,
          ticks: {
            color: '#667',
            callback: function(v) { return formatBytes(v); }
          },
          grid: { color: '#2a2a4a' }
        }
      }
    }
  });
  } catch(e) {
    document.getElementById('driveList').innerHTML = '<div class="card dim">Error loading drives</div>';
  }
}

function formatBytes(b) {
  if (!b) return '0 B';
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let v = b, i = 0;
  while (v >= 1024 && i < 4) { v /= 1024; i++; }
  return v.toFixed(1) + ' ' + u[i];
}

async function refreshAll() {
  var p = await Promise.all([api('/api/v1/tiers'),api('/api/v1/savings'),api('/api/v1/cycle/history?n=20'),api('/api/v1/engine'),api('/api/v1/drives')]);
  renderTiers(p[0]); renderSavings(p[1]); renderMigrations(p[2]); renderStatus(p[3],p[0]); renderOverviewDrives(p[4]);
  // Reload drives if drives tab is visible
  if (document.getElementById('panel-drives').classList.contains('active')) loadDrives();
}
function startAutoRefresh(){refreshAll();if(refreshTimer)clearInterval(refreshTimer);refreshTimer=setInterval(refreshAll,REFRESH_MS);}
document.addEventListener('DOMContentLoaded',startAutoRefresh);
</script>
</body>
</html>
)raw");
    auto pos = html.find("__API_KEY__");
    if (pos != std::string::npos)
        html.replace(pos, 11, api_key);
    return html;
}
