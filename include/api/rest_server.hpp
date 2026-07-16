#pragma once

#include "api/auth.hpp"
#include "api/fileserver.hpp"
#include "core/file_record.hpp"
#include "catalog/catalog_interface.hpp"
#include "policy/policy_engine.hpp"
#include "migration/migration_engine.hpp"
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <optional>

class RestServer {
public:
    RestServer(std::shared_ptr<CatalogInterface> catalog,
               std::shared_ptr<PolicyEngine> policy,
               std::shared_ptr<MigrationEngine> engine);

    bool start(int port = 3000, const std::string& api_key = "",
               const std::string& jwt_secret = "");
    void stop();
    bool is_running() const { return running_.load(); }
    int  port() const { return port_; }

    void set_allowed_origins(const std::string& origins);
    AuthManager& auth() { return auth_; }

private:
    std::shared_ptr<CatalogInterface> catalog_;
    std::shared_ptr<PolicyEngine>      policy_;
    std::shared_ptr<MigrationEngine>   engine_;
    std::unique_ptr<FileServer>        file_server_;

    std::atomic<bool> running_{false};
    std::thread server_thread_;
    int port_ = 3000;
    AuthManager auth_;

    struct AuthContext {
        std::string user_id;
        std::string username;
        UserRole role = UserRole::USER;
        bool authenticated = false;
        bool is_admin = false;
    };

    void server_loop(int port, const std::string& api_key);

    // Route handlers (accept AuthContext for per-user scoping)
    std::string handle_list_files(const std::string& query, const AuthContext& ctx);
    std::string handle_get_file(const std::string& id, const AuthContext& ctx);
    std::string handle_touch_file(const std::string& id, const AuthContext& ctx);
    std::string handle_pin_file(const std::string& id, bool pin, const AuthContext& ctx);
    std::string handle_get_drives();
    std::string handle_get_tiers();
    std::string handle_run_cycle();
    std::string handle_get_history(int n = 20);
    std::string handle_get_analysis();
    std::string handle_get_savings();
    std::string handle_get_metrics();
    std::string handle_reload_config();
    std::string handle_list_by_tier(int tier_idx);

    // Auth handlers
    std::string handle_auth_register(const std::string& body);
    std::string handle_auth_login(const std::string& body);
    std::string handle_auth_me(const AuthContext& ctx);
    std::string handle_auth_users();
    std::string handle_auth_delete_user(const std::string& user_id);

    // Helpers
    std::string json_response(int status, const std::string& body);
    std::string error_response(int status, const std::string& message);
    std::string html_dashboard(const std::string& api_key = "");
    std::string file_record_to_json(const FileRecord& f);
    std::string migration_event_to_json(const MigrationEvent& e);
};
