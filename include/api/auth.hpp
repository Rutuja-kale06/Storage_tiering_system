#pragma once

#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <ctime>
#include <nlohmann/json.hpp>

class SupabaseClient;

enum class UserRole : int {
    ADMIN    = 0,
    OPERATOR = 1,
    VIEWER   = 2,
    USER     = 3
};

inline constexpr const char* USER_ROLE_NAMES[] = {
    "admin", "operator", "viewer", "user"
};

inline std::string user_role_name(UserRole r) {
    int idx = static_cast<int>(r);
    return (idx >= 0 && idx < 4) ? USER_ROLE_NAMES[idx] : "user";
}

inline UserRole user_role_from_string(const std::string& s) {
    if (s == "admin") return UserRole::ADMIN;
    if (s == "operator") return UserRole::OPERATOR;
    if (s == "viewer") return UserRole::VIEWER;
    return UserRole::USER;
}

struct User {
    std::string id;
    std::string username;
    std::string password_hash;
    std::string display_name;
    UserRole    role = UserRole::USER;
    time_t      created_at = 0;
    time_t      last_login = 0;
};

struct JwtPayload {
    std::string user_id;
    std::string username;
    UserRole    role = UserRole::USER;
    time_t      iat = 0;
    time_t      exp = 0;
};

class AuthManager {
public:
    AuthManager();

    void set_supabase(SupabaseClient* client) { supabase_ = client; }

    void set_secret(const std::string& secret) { secret_ = secret; }
    std::string secret() const { return secret_; }

    std::string hash_password(const std::string& password) const;
    bool verify_password(const std::string& password, const std::string& hash) const;

    std::string create_token(const std::string& user_id,
                             const std::string& username,
                             UserRole role,
                             int expiry_hours = 24) const;
    std::optional<JwtPayload> verify_token(const std::string& token) const;

    struct UserResult {
        bool success = false;
        std::string error;
        std::optional<User> user;
    };

    UserResult register_user(const std::string& username,
                             const std::string& password,
                             const std::string& display_name,
                             UserRole role = UserRole::USER);
    UserResult login_user(const std::string& username, const std::string& password);
    UserResult get_user(const std::string& user_id) const;
    std::vector<User> list_users() const;
    bool delete_user(const std::string& user_id);
    bool update_user_role(const std::string& user_id, UserRole role);

    void save_users(const std::string& path);
    void load_users(const std::string& path);
    void ensure_default_admin(const std::string& path);

private:
    std::string secret_;
    SupabaseClient* supabase_ = nullptr;

    static std::string base64url_encode(const unsigned char* data, size_t len);
    static std::vector<unsigned char> base64url_decode(const std::string& str);
    std::vector<unsigned char> hmac_sha256(const std::string& data) const;
    static std::vector<unsigned char> random_bytes(size_t count);

    struct UserStore {
        std::vector<User> users;
        std::mutex mtx;
    };
    static UserStore& store();
    static std::string generate_id();
    static User* find_user_by_name(const std::string& username);
    static User* find_user_by_id(const std::string& id);

    // Supabase helpers
    UserResult register_user_supabase(const std::string& username,
                                       const std::string& password,
                                       const std::string& display_name,
                                       UserRole role);
    UserResult login_user_supabase(const std::string& username,
                                    const std::string& password);
    UserResult get_user_supabase(const std::string& user_id) const;
    std::vector<User> list_users_supabase() const;
    bool delete_user_supabase(const std::string& user_id);
    bool update_user_role_supabase(const std::string& user_id, UserRole role);

    User row_to_user(const nlohmann::json& j) const;
};
