#pragma once

#include <string>
#include <functional>
#include <optional>
#include <map>

struct SupabaseConfig {
    std::string project_url;
    std::string anon_key;
    bool enabled = false;
};

struct SupabaseUser {
    std::string id;
    std::string email;
    std::string display_name;
    std::string role;
};

struct SupabaseAuthResult {
    bool success = false;
    std::string error;
    std::string access_token;
    std::string refresh_token;
    std::optional<SupabaseUser> user;
};

class SupabaseClient {
public:
    SupabaseClient();

    void configure(const SupabaseConfig& config);
    bool is_configured() const { return configured_; }
    const SupabaseConfig& config() const { return config_; }

    SupabaseAuthResult sign_up(const std::string& email,
                                const std::string& password,
                                const std::string& display_name = "");
    SupabaseAuthResult sign_in(const std::string& email,
                                const std::string& password);
    SupabaseAuthResult sign_in_with_token(const std::string& refresh_token);

    std::optional<SupabaseUser> get_user(const std::string& access_token);

    // Data API: query rows from a table
    std::optional<std::string> query(const std::string& table,
                                      const std::string& select,
                                      const std::string& filter,
                                      const std::string& access_token = "");

    // Data API: insert row
    bool insert(const std::string& table,
                const std::string& json_body,
                const std::string& access_token = "");

    // Data API: update rows
    bool update(const std::string& table,
                const std::string& json_body,
                const std::string& filter,
                const std::string& access_token = "");

    // Data API: delete rows
    bool remove(const std::string& table,
                const std::string& filter,
                const std::string& access_token = "");

    // Fetch user info from the jwt token (decode without verifying)
    static std::string user_id_from_token(const std::string& access_token);

    // Convert a username to a Supabase-compatible email (username@fileserver.local)
    static std::string username_to_email(const std::string& username);

private:
    SupabaseConfig config_;
    bool configured_ = false;

    std::string auth_url(const std::string& path) const;
    std::string rest_url(const std::string& table) const;

    std::string http_post(const std::string& url,
                           const std::string& body,
                           const std::map<std::string, std::string>& headers);
    std::string http_get(const std::string& url,
                          const std::map<std::string, std::string>& headers);
};
