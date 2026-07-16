#include "api/auth.hpp"
#include "api/supabase_client.hpp"
#include "logger.hpp"
#include <vector>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <algorithm>
#include <random>
#include <cstring>
#include <fstream>
#include <windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

// ── Base64url ────────────────────────────────────────────

std::string AuthManager::base64url_encode(const unsigned char* data, size_t len) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned b = (data[i] & 0xFF) << 16;
        if (i + 1 < len) b |= (data[i + 1] & 0xFF) << 8;
        if (i + 2 < len) b |= (data[i + 2] & 0xFF);
        out.push_back(b64[(b >> 18) & 0x3F]);
        out.push_back(b64[(b >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? b64[(b >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? b64[b & 0x3F] : '=');
    }
    for (auto& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

std::vector<unsigned char> AuthManager::base64url_decode(const std::string& str) {
    std::string normalized = str;
    for (auto& c : normalized) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (normalized.size() % 4) normalized.push_back('=');
    static const std::string b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> out;
    out.reserve((normalized.size() / 4) * 3);
    for (size_t i = 0; i < normalized.size(); i += 4) {
        if (normalized[i] == '=') break;
        unsigned b = b64.find(normalized[i]) << 18;
        b |= b64.find(normalized[i + 1]) << 12;
        b |= (normalized[i + 2] != '=' ? b64.find(normalized[i + 2]) << 6 : 0);
        b |= (normalized[i + 3] != '=' ? b64.find(normalized[i + 3]) : 0);
        out.push_back((b >> 16) & 0xFF);
        if (normalized[i + 2] != '=') out.push_back((b >> 8) & 0xFF);
        if (normalized[i + 3] != '=') out.push_back(b & 0xFF);
    }
    return out;
}

// ── Random bytes ─────────────────────────────────────────

std::vector<unsigned char> AuthManager::random_bytes(size_t count) {
    std::vector<unsigned char> buf(count);
    HCRYPTPROV hProv = 0;
    if (CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(hProv, (DWORD)count, buf.data());
        CryptReleaseContext(hProv, 0);
    }
    return buf;
}

// ── HMAC-SHA256 ──────────────────────────────────────────

static std::vector<unsigned char> sha256_raw(const unsigned char* data, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    std::vector<unsigned char> result;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return result;

    DWORD hashLen = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &hashLen, 0);
    DWORD objLen = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &objLen, 0);

    std::vector<unsigned char> obj(objLen);
    result.resize(hashLen);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, obj.data(), (ULONG)obj.size(), nullptr, 0, 0) == 0) {
        BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0);
        BCryptFinishHash(hHash, result.data(), (ULONG)result.size(), 0);
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
}

std::vector<unsigned char> AuthManager::hmac_sha256(const std::string& data) const {
    const size_t BLOCK_SIZE = 64;
    std::vector<unsigned char> key(BLOCK_SIZE, 0);

    size_t key_len = secret_.size();
    if (key_len > BLOCK_SIZE) {
        auto hashed = sha256_raw((unsigned char*)secret_.data(), secret_.size());
        if (hashed.empty()) return {};
        std::copy(hashed.begin(), hashed.end(), key.begin());
    } else {
        std::copy(secret_.begin(), secret_.end(), key.begin());
    }

    std::vector<unsigned char> inner_input;
    inner_input.reserve(BLOCK_SIZE + data.size());
    for (size_t i = 0; i < BLOCK_SIZE; ++i)
        inner_input.push_back(key[i] ^ 0x36);
    inner_input.insert(inner_input.end(), data.begin(), data.end());

    auto inner_hash = sha256_raw(inner_input.data(), inner_input.size());
    if (inner_hash.empty()) return {};

    std::vector<unsigned char> outer_input;
    outer_input.reserve(BLOCK_SIZE + 32);
    for (size_t i = 0; i < BLOCK_SIZE; ++i)
        outer_input.push_back(key[i] ^ 0x5c);
    outer_input.insert(outer_input.end(), inner_hash.begin(), inner_hash.end());

    return sha256_raw(outer_input.data(), outer_input.size());
}

// ── Password hashing (PBKDF2-SHA256) ─────────────────────

std::string AuthManager::hash_password(const std::string& password) const {
    auto salt = random_bytes(16);
    std::vector<unsigned char> hash(32);
    int iterations = 10000;

    std::vector<unsigned char> prev(32, 0);
    std::vector<unsigned char> combined = salt;
    combined.push_back(0); combined.push_back(0); combined.push_back(0); combined.push_back(1);

    {
        BCRYPT_ALG_HANDLE hAlg2 = nullptr;
        BCRYPT_HASH_HANDLE hHash = nullptr;
        if (BCryptOpenAlgorithmProvider(&hAlg2, BCRYPT_SHA256_ALGORITHM, nullptr,
                                         BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0) {
            DWORD objLen = 0, hLen = 0;
            BCryptGetProperty(hAlg2, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &objLen, 0);
            BCryptGetProperty(hAlg2, BCRYPT_HASH_LENGTH, (PUCHAR)&hLen, sizeof(hLen), &hLen, 0);
            std::vector<unsigned char> obj(objLen);
            if (BCryptCreateHash(hAlg2, &hHash, obj.data(), (ULONG)obj.size(),
                                  (PUCHAR)password.data(), (ULONG)password.size(), 0) == 0) {
                BCryptHashData(hHash, combined.data(), (ULONG)combined.size(), 0);
                BCryptFinishHash(hHash, prev.data(), (ULONG)prev.size(), 0);
                BCryptDestroyHash(hHash);
            }
            BCryptCloseAlgorithmProvider(hAlg2, 0);
        }
    }

    hash = prev;

    for (int i = 1; i < iterations; ++i) {
        std::vector<unsigned char> next(32);
        BCRYPT_ALG_HANDLE hAlg2 = nullptr;
        BCRYPT_HASH_HANDLE hHash = nullptr;
        if (BCryptOpenAlgorithmProvider(&hAlg2, BCRYPT_SHA256_ALGORITHM, nullptr,
                                         BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0) {
            DWORD objLen = 0, hLen = 0;
            BCryptGetProperty(hAlg2, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &objLen, 0);
            BCryptGetProperty(hAlg2, BCRYPT_HASH_LENGTH, (PUCHAR)&hLen, sizeof(hLen), &hLen, 0);
            std::vector<unsigned char> obj(objLen);
            if (BCryptCreateHash(hAlg2, &hHash, obj.data(), (ULONG)obj.size(),
                                  (PUCHAR)password.data(), (ULONG)password.size(), 0) == 0) {
                BCryptHashData(hHash, prev.data(), (ULONG)prev.size(), 0);
                BCryptFinishHash(hHash, next.data(), (ULONG)next.size(), 0);
                BCryptDestroyHash(hHash);
            }
            BCryptCloseAlgorithmProvider(hAlg2, 0);
        }
        for (size_t j = 0; j < hash.size(); ++j)
            hash[j] ^= next[j];
        prev = next;
    }

    return base64url_encode(salt.data(), salt.size()) + ":" +
           std::to_string(iterations) + ":" +
           base64url_encode(hash.data(), hash.size());
}

bool AuthManager::verify_password(const std::string& password, const std::string& hash) const {
    auto parts = std::vector<std::string>();
    std::stringstream ss(hash);
    std::string item;
    while (std::getline(ss, item, ':')) parts.push_back(item);
    if (parts.size() != 3) return false;

    auto salt = base64url_decode(parts[0]);
    int iterations = std::stoi(parts[1]);
    auto expected = base64url_decode(parts[2]);

    std::vector<unsigned char> prev(32, 0);
    std::vector<unsigned char> combined = salt;
    combined.push_back(0); combined.push_back(0); combined.push_back(0); combined.push_back(1);

    {
        BCRYPT_ALG_HANDLE hAlg = nullptr;
        BCRYPT_HASH_HANDLE hHash = nullptr;
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                         BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0) {
            DWORD objLen = 0, hLen = 0;
            BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &objLen, 0);
            BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hLen, sizeof(hLen), &hLen, 0);
            std::vector<unsigned char> obj(objLen);
            if (BCryptCreateHash(hAlg, &hHash, obj.data(), (ULONG)obj.size(),
                                  (PUCHAR)password.data(), (ULONG)password.size(), 0) == 0) {
                BCryptHashData(hHash, combined.data(), (ULONG)combined.size(), 0);
                BCryptFinishHash(hHash, prev.data(), (ULONG)prev.size(), 0);
                BCryptDestroyHash(hHash);
            }
            BCryptCloseAlgorithmProvider(hAlg, 0);
        }
    }

    std::vector<unsigned char> computed(32, 0);
    for (size_t j = 0; j < 32; ++j) computed[j] = prev[j];

    for (int i = 1; i < iterations; ++i) {
        std::vector<unsigned char> next(32);
        BCRYPT_ALG_HANDLE hAlg = nullptr;
        BCRYPT_HASH_HANDLE hHash = nullptr;
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                         BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0) {
            DWORD objLen = 0, hLen = 0;
            BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &objLen, 0);
            BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hLen, sizeof(hLen), &hLen, 0);
            std::vector<unsigned char> obj(objLen);
            if (BCryptCreateHash(hAlg, &hHash, obj.data(), (ULONG)obj.size(),
                                  (PUCHAR)password.data(), (ULONG)password.size(), 0) == 0) {
                BCryptHashData(hHash, prev.data(), (ULONG)prev.size(), 0);
                BCryptFinishHash(hHash, next.data(), (ULONG)next.size(), 0);
                BCryptDestroyHash(hHash);
            }
            BCryptCloseAlgorithmProvider(hAlg, 0);
        }
        for (size_t j = 0; j < computed.size(); ++j)
            computed[j] ^= next[j];
        prev = next;
    }

    return computed == expected;
}

// ── JWT ──────────────────────────────────────────────────

std::string AuthManager::create_token(const std::string& user_id,
                                       const std::string& username,
                                       UserRole role,
                                       int expiry_hours) const {
    auto now = std::time(nullptr);
    nlohmann::json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    nlohmann::json payload = {
        {"sub", user_id},
        {"name", username},
        {"role", user_role_name(role)},
        {"iat", (int64_t)now},
        {"exp", (int64_t)(now + expiry_hours * 3600)}
    };

    std::string hdr_b64 = base64url_encode(
        (unsigned char*)header.dump().data(), header.dump().size());
    std::string pld_b64 = base64url_encode(
        (unsigned char*)payload.dump().data(), payload.dump().size());

    std::string signing_input = hdr_b64 + "." + pld_b64;
    auto sig = hmac_sha256(signing_input);
    std::string sig_b64 = base64url_encode(sig.data(), sig.size());

    return signing_input + "." + sig_b64;
}

std::optional<JwtPayload> AuthManager::verify_token(const std::string& token) const {
    // If Supabase is configured, try decoding the Supabase JWT directly
    if (supabase_ && supabase_->is_configured()) {
        // Supabase JWTs are signed by Supabase; we trust them
        auto user_id = SupabaseClient::user_id_from_token(token);
        if (!user_id.empty()) {
            JwtPayload p;
            p.user_id = user_id;
            p.username = user_id;
            p.role = UserRole::USER;
            p.iat = 0;
            p.exp = 9999999999;
            return p;
        }
    }

    // Fallback to local JWT verification
    auto dots = std::count(token.begin(), token.end(), '.');
    if (dots != 2) return std::nullopt;

    auto dot1 = token.find('.');
    auto dot2 = token.rfind('.');
    std::string hdr_b64 = token.substr(0, dot1);
    std::string pld_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string sig_b64 = token.substr(dot2 + 1);

    std::string signing_input = hdr_b64 + "." + pld_b64;
    auto expected_sig = hmac_sha256(signing_input);
    auto actual_sig = base64url_decode(sig_b64);

    if (expected_sig.size() != actual_sig.size()) return std::nullopt;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < expected_sig.size(); ++i)
        diff |= expected_sig[i] ^ actual_sig[i];
    if (diff != 0) return std::nullopt;

    auto pld_json = base64url_decode(pld_b64);
    try {
        auto j = nlohmann::json::parse(std::string(pld_json.begin(), pld_json.end()));
        JwtPayload p;
        p.user_id = j.value("sub", "");
        p.username = j.value("name", "");
        p.role = user_role_from_string(j.value("role", "user"));
        p.iat = j.value("iat", (int64_t)0);
        p.exp = j.value("exp", (int64_t)0);

        if (p.user_id.empty()) return std::nullopt;
        if (std::time(nullptr) > p.exp) return std::nullopt;

        return p;
    } catch (...) {
        return std::nullopt;
    }
}

// ── User Store ───────────────────────────────────────────

AuthManager::UserStore& AuthManager::store() {
    static UserStore s;
    return s;
}

std::string AuthManager::generate_id() {
    auto r = random_bytes(16);
    return base64url_encode(r.data(), r.size());
}

User* AuthManager::find_user_by_name(const std::string& username) {
    auto& s = store();
    for (auto& u : s.users)
        if (u.username == username) return &u;
    return nullptr;
}

User* AuthManager::find_user_by_id(const std::string& id) {
    auto& s = store();
    for (auto& u : s.users)
        if (u.id == id) return &u;
    return nullptr;
}

AuthManager::AuthManager() {
    if (secret_.empty()) {
        auto r = random_bytes(32);
        secret_ = base64url_encode(r.data(), r.size());
    }
}

// ── Supabase-backed helpers ──────────────────────────────

User AuthManager::row_to_user(const nlohmann::json& j) const {
    User u;
    u.id = j.value("id", "");
    u.username = j.value("username", "");
    u.password_hash = j.value("password_hash", "");
    u.display_name = j.value("display_name", "");
    u.role = user_role_from_string(j.value("role", "user"));
    u.created_at = j.value("created_at", (int64_t)0);
    u.last_login = j.value("last_login", (int64_t)0);
    return u;
}

AuthManager::UserResult AuthManager::register_user_supabase(
    const std::string& username,
    const std::string& password,
    const std::string& display_name,
    UserRole role)
{
    UserResult result;
    if (!supabase_ || !supabase_->is_configured()) {
        result.error = "Supabase not configured";
        return result;
    }

    // Sign up via Supabase Auth — generate email from username
    std::string email = SupabaseClient::username_to_email(username);
    auto auth_result = supabase_->sign_up(email, password, display_name);
    if (!auth_result.success) {
        result.error = auth_result.error;
        return result;
    }

    // Hash password locally for our users table
    std::string pw_hash = hash_password(password);
    std::string user_id = auth_result.user.has_value() ? auth_result.user->id : generate_id();

    // Insert into the users table via Data API
    nlohmann::json user_j;
    user_j["id"] = user_id;
    user_j["username"] = username;
    user_j["password_hash"] = pw_hash;
    user_j["display_name"] = display_name.empty() ? username : display_name;
    user_j["role"] = user_role_name(role);
    user_j["created_at"] = (int64_t)std::time(nullptr);
    user_j["last_login"] = 0;

    if (supabase_->insert("users", user_j.dump(), auth_result.access_token)) {
        User u = row_to_user(user_j);
        result.success = true;
        result.user = u;
        LOG_INFO("Auth", "Supabase user registered: " + username);
    } else {
        result.error = "Failed to create user record";
    }
    return result;
}

AuthManager::UserResult AuthManager::login_user_supabase(
    const std::string& username,
    const std::string& password)
{
    UserResult result;
    if (!supabase_ || !supabase_->is_configured()) {
        result.error = "Supabase not configured";
        return result;
    }

    // Supabase Auth requires email, not username — generate email from username
    std::string email = SupabaseClient::username_to_email(username);
    auto auth_result = supabase_->sign_in(email, password);
    if (!auth_result.success) {
        result.error = auth_result.error;
        return result;
    }

    // Fetch user data from the users table
    std::string user_id = auth_result.user.has_value() ? auth_result.user->id : "";
    if (user_id.empty()) {
        result.error = "Invalid user ID from auth";
        return result;
    }

    auto users_json = supabase_->query("users", "*", "id=eq." + user_id, auth_result.access_token);
    if (users_json.has_value()) {
        try {
            auto arr = nlohmann::json::parse(*users_json);
            if (arr.is_array() && !arr.empty()) {
                User u = row_to_user(arr[0]);
                u.last_login = std::time(nullptr);
                result.success = true;
                result.user = u;

                // Update last_login in Supabase
                nlohmann::json update_j;
                update_j["last_login"] = (int64_t)u.last_login;
                supabase_->update("users", update_j.dump(), "id=eq." + user_id, auth_result.access_token);

                LOG_INFO("Auth", "Supabase user logged in: " + username);
                return result;
            }
        } catch (const std::exception& e) {
            result.error = std::string("Parse error: ") + e.what();
            return result;
        }
    }

    // If no local user record, create one from auth data
    nlohmann::json user_j;
    user_j["id"] = user_id;
    user_j["username"] = username;
    user_j["password_hash"] = hash_password(password);
    user_j["display_name"] = username;
    user_j["role"] = "user";
    user_j["created_at"] = (int64_t)std::time(nullptr);
    user_j["last_login"] = (int64_t)std::time(nullptr);

    if (supabase_->insert("users", user_j.dump(), auth_result.access_token)) {
        User u = row_to_user(user_j);
        result.success = true;
        result.user = u;
        LOG_INFO("Auth", "Auto-created user record: " + username);
    } else {
        result.error = "Failed to create user record";
    }
    return result;
}

AuthManager::UserResult AuthManager::get_user_supabase(const std::string& user_id) const {
    UserResult result;
    if (!supabase_ || !supabase_->is_configured()) {
        result.error = "Supabase not configured";
        return result;
    }

    auto users_json = supabase_->query("users", "*", "id=eq." + user_id);
    if (!users_json.has_value()) {
        result.error = "User not found";
        return result;
    }

    try {
        auto arr = nlohmann::json::parse(*users_json);
        if (arr.is_array() && !arr.empty()) {
            result.success = true;
            result.user = row_to_user(arr[0]);
        } else {
            result.error = "User not found";
        }
    } catch (const std::exception& e) {
        result.error = std::string("Parse error: ") + e.what();
    }
    return result;
}

std::vector<User> AuthManager::list_users_supabase() const {
    std::vector<User> users;
    if (!supabase_ || !supabase_->is_configured()) return users;

    auto users_json = supabase_->query("users", "*", "");
    if (!users_json.has_value()) return users;

    try {
        auto arr = nlohmann::json::parse(*users_json);
        if (arr.is_array()) {
            for (const auto& j : arr)
                users.push_back(row_to_user(j));
        }
    } catch (...) {}
    return users;
}

bool AuthManager::delete_user_supabase(const std::string& user_id) {
    if (!supabase_ || !supabase_->is_configured()) return false;
    return supabase_->remove("users", "id=eq." + user_id);
}

bool AuthManager::update_user_role_supabase(const std::string& user_id, UserRole role) {
    if (!supabase_ || !supabase_->is_configured()) return false;
    nlohmann::json j;
    j["role"] = user_role_name(role);
    return supabase_->update("users", j.dump(), "id=eq." + user_id);
}

// ── Public API ───────────────────────────────────────────

AuthManager::UserResult AuthManager::register_user(
    const std::string& username,
    const std::string& password,
    const std::string& display_name,
    UserRole role)
{
    if (supabase_ && supabase_->is_configured()) {
        auto result = register_user_supabase(username, password, display_name, role);
        if (result.success) return result;
    }

    // Fallback: local user store
    UserResult r;
    if (username.empty() || password.empty()) {
        r.error = "Username and password required";
        return r;
    }
    if (username.size() > 64) {
        r.error = "Username must be 64 characters or fewer";
        return r;
    }
    if (password.size() < 6) {
        r.error = "Password must be at least 6 characters";
        return r;
    }
    if (password.size() > 128) {
        r.error = "Password must be 128 characters or fewer";
        return r;
    }

    auto& s = store();
    std::lock_guard<std::mutex> lk(s.mtx);

    if (find_user_by_name(username)) {
        r.error = "Username already exists";
        return r;
    }

    User u;
    u.id = generate_id();
    u.username = username;
    u.password_hash = hash_password(password);
    u.display_name = display_name.empty() ? username : display_name;
    u.role = role;
    u.created_at = std::time(nullptr);
    u.last_login = 0;

    s.users.push_back(u);
    r.success = true;
    r.user = u;
    return r;
}

AuthManager::UserResult AuthManager::login_user(
    const std::string& username,
    const std::string& password)
{
    if (supabase_ && supabase_->is_configured()) {
        auto result = login_user_supabase(username, password);
        if (result.success) return result;
    }

    // Fallback: local user store
    UserResult r;
    auto& s = store();
    std::lock_guard<std::mutex> lk(s.mtx);

    auto* user = find_user_by_name(username);
    if (!user) {
        r.error = "Invalid username or password";
        return r;
    }

    if (!verify_password(password, user->password_hash)) {
        r.error = "Invalid username or password";
        return r;
    }

    user->last_login = std::time(nullptr);
    r.success = true;
    r.user = *user;
    return r;
}

AuthManager::UserResult AuthManager::get_user(const std::string& user_id) const {
    if (supabase_ && supabase_->is_configured())
        return get_user_supabase(user_id);

    UserResult r;
    auto& s = store();
    std::lock_guard<std::mutex> lk(s.mtx);
    auto* user = find_user_by_id(user_id);
    if (!user) {
        r.error = "User not found";
        return r;
    }
    r.success = true;
    r.user = *user;
    return r;
}

std::vector<User> AuthManager::list_users() const {
    if (supabase_ && supabase_->is_configured())
        return list_users_supabase();

    auto& s = store();
    std::lock_guard<std::mutex> lk(s.mtx);
    return s.users;
}

bool AuthManager::delete_user(const std::string& user_id) {
    if (supabase_ && supabase_->is_configured())
        return delete_user_supabase(user_id);

    auto& s = store();
    std::lock_guard<std::mutex> lk(s.mtx);
    auto* user = find_user_by_id(user_id);
    if (!user) return false;
    s.users.erase(std::remove_if(s.users.begin(), s.users.end(),
        [&](const User& u) { return u.id == user_id; }), s.users.end());
    return true;
}

bool AuthManager::update_user_role(const std::string& user_id, UserRole role) {
    if (supabase_ && supabase_->is_configured())
        return update_user_role_supabase(user_id, role);

    auto& s = store();
    std::lock_guard<std::mutex> lk(s.mtx);
    auto* user = find_user_by_id(user_id);
    if (!user) return false;
    user->role = role;
    return true;
}

// ── Persistence (local fallback only) ────────────────────

void AuthManager::save_users(const std::string& path) {
    auto& s = store();
    std::lock_guard<std::mutex> lk(s.mtx);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& u : s.users) {
        arr.push_back({
            {"id", u.id},
            {"username", u.username},
            {"password_hash", u.password_hash},
            {"display_name", u.display_name},
            {"role", user_role_name(u.role)},
            {"created_at", u.created_at},
            {"last_login", u.last_login}
        });
    }
    nlohmann::json j;
    j["users"] = arr;
    std::ofstream f(path);
    if (f.is_open()) f << j.dump(2);
}

void AuthManager::load_users(const std::string& path) {
    auto& s = store();
    std::lock_guard<std::mutex> lk(s.mtx);
    std::ifstream f(path);
    if (!f.is_open()) return;
    nlohmann::json j;
    try { f >> j; } catch (...) { return; }
    if (!j.contains("users")) return;
    s.users.clear();
    for (const auto& uj : j["users"]) {
        User u;
        u.id = uj.value("id", "");
        u.username = uj.value("username", "");
        u.password_hash = uj.value("password_hash", "");
        u.display_name = uj.value("display_name", "");
        u.role = user_role_from_string(uj.value("role", "user"));
        u.created_at = uj.value("created_at", (int64_t)0);
        u.last_login = uj.value("last_login", (int64_t)0);
        if (!u.username.empty() && !u.password_hash.empty())
            s.users.push_back(u);
    }
}

void AuthManager::ensure_default_admin(const std::string& path) {
    // If Supabase is configured, create default admin user there
    if (supabase_ && supabase_->is_configured()) {
        auto users = list_users_supabase();
        if (!users.empty()) return;

        auto auth_result = supabase_->sign_up(
            SupabaseClient::username_to_email("admin"), "admin123", "Administrator");
        if (auth_result.success) {
            std::string admin_id = auth_result.user.has_value()
                ? auth_result.user->id : generate_id();

            nlohmann::json user_j;
            user_j["id"] = admin_id;
            user_j["username"] = "admin";
            user_j["password_hash"] = hash_password("admin123");
            user_j["display_name"] = "Administrator";
            user_j["role"] = "admin";
            user_j["created_at"] = (int64_t)std::time(nullptr);
            user_j["last_login"] = 0;

            supabase_->insert("users", user_j.dump(), auth_result.access_token);
            LOG_INFO("Auth", "Default admin created in Supabase (admin/admin123)");
        }
        // Fall through to local user store if Supabase failed
    }

    // Fallback: local user store
    load_users(path);
    {
        auto& s = store();
        std::lock_guard<std::mutex> lk(s.mtx);
        if (!s.users.empty()) return;
    }
    auto result = register_user("admin", "admin123", "Administrator", UserRole::ADMIN);
    if (result.success) {
        save_users(path);
    }
}
