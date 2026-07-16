#include "api/supabase_client.hpp"
#include "logger.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>
#include <windows.h>
#include <winhttp.h>

SupabaseClient::SupabaseClient() {}

void SupabaseClient::configure(const SupabaseConfig& config) {
    config_ = config;
    configured_ = config.enabled && !config.project_url.empty() && !config.anon_key.empty();
    if (configured_) {
        LOG_INFO("Supabase", "Client configured: " + config_.project_url);
    }
}

std::string SupabaseClient::auth_url(const std::string& path) const {
    return config_.project_url + "/auth/v1/" + path;
}

std::string SupabaseClient::rest_url(const std::string& table) const {
    return config_.project_url + "/rest/v1/" + table;
}

static std::wstring to_wstring(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}

static std::string to_string(const std::wstring& w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, nullptr, nullptr);
    return s;
}

struct ParsedUrl {
    std::wstring host;
    int port;
    std::wstring path;
};

static bool parse_url(const std::string& url_str, ParsedUrl& out) {
    URL_COMPONENTSW uc = { sizeof(uc) };
    wchar_t scheme[32], host[256], path[4096];
    uc.lpszScheme = scheme; uc.dwSchemeLength = 32;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    uc.nPort = 0;

    auto wurl = to_wstring(url_str);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc))
        return false;

    out.host = host;
    out.port = uc.nPort == 0 ? (uc.nScheme == INTERNET_SCHEME_HTTPS ? 443 : 80) : uc.nPort;
    out.path = path;
    return true;
}

struct WinHttpResponse {
    int status_code = 0;
    std::string body;
};

static WinHttpResponse winhttp_request(
    const std::string& method,
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& headers)
{
    WinHttpResponse resp;
    ParsedUrl parsed;
    if (!parse_url(url, parsed)) {
        LOG_ERROR("Supabase", "Failed to parse URL: " + url);
        return resp;
    }

    HINTERNET hSession = WinHttpOpen(L"SupabaseClient/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        LOG_ERROR("Supabase", "WinHttpOpen failed");
        return resp;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, parsed.host.c_str(),
        (INTERNET_PORT)parsed.port, 0);
    if (!hConnect) {
        LOG_ERROR("Supabase", "WinHttpConnect failed");
        WinHttpCloseHandle(hSession);
        return resp;
    }

    DWORD flags = parsed.port == 443 ? WINHTTP_FLAG_SECURE : 0;
    auto wmethod = to_wstring(method);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, wmethod.c_str(),
        parsed.path.c_str(), NULL, NULL, NULL, flags);
    if (!hRequest) {
        LOG_ERROR("Supabase", "WinHttpOpenRequest failed");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return resp;
    }

    // Set headers
    for (const auto& [k, v] : headers) {
        std::string hdr = k + ": " + v;
        auto whdr = to_wstring(hdr);
        WinHttpAddRequestHeaders(hRequest, whdr.c_str(), (DWORD)whdr.size(),
            WINHTTP_ADDREQ_FLAG_ADD);
    }

    // Send request
    DWORD bodyLen = (DWORD)body.size();
    void* bodyPtr = bodyLen > 0 ? (void*)body.data() : NULL;
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        bodyPtr, bodyLen, bodyLen, 0))
    {
        LOG_ERROR("Supabase", "WinHttpSendRequest failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return resp;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        LOG_ERROR("Supabase", "WinHttpReceiveResponse failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return resp;
    }

    // Read status code
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        NULL, &statusCode, &statusSize, NULL);
    resp.status_code = (int)statusCode;

    // Read body
    char buffer[8192];
    DWORD bytesRead = 0;
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        resp.body.append(buffer, bytesRead);
        bytesRead = 0;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return resp;
}

std::string SupabaseClient::http_post(
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& headers)
{
    std::map<std::string, std::string> all_headers = headers;
    all_headers["Content-Type"] = "application/json";
    all_headers["apikey"] = config_.anon_key;

    auto resp = winhttp_request("POST", url, body, all_headers);

    if (resp.status_code >= 200 && resp.status_code < 300) {
        return resp.body;
    }
    if (resp.status_code > 0) {
        LOG_ERROR("Supabase", "POST failed (" + std::to_string(resp.status_code) + "): " + resp.body);
        return resp.body;
    }
    LOG_ERROR("Supabase", "POST failed: no response");
    return "";
}

std::string SupabaseClient::http_get(
    const std::string& url,
    const std::map<std::string, std::string>& headers)
{
    std::map<std::string, std::string> all_headers = headers;
    all_headers["apikey"] = config_.anon_key;

    auto resp = winhttp_request("GET", url, "", all_headers);

    if (resp.status_code >= 200 && resp.status_code < 300) {
        return resp.body;
    }
    if (resp.status_code > 0) {
        LOG_ERROR("Supabase", "GET failed (" + std::to_string(resp.status_code) + "): " + resp.body);
        return resp.body;
    }
    LOG_ERROR("Supabase", "GET failed: no response");
    return "";
}

SupabaseAuthResult SupabaseClient::sign_up(
    const std::string& email,
    const std::string& password,
    const std::string& display_name)
{
    SupabaseAuthResult result;
    if (!configured_) { result.error = "Supabase not configured"; return result; }

    nlohmann::json j;
    j["email"] = email;
    j["password"] = password;
    if (!display_name.empty())
        j["data"] = {{"display_name", display_name}};

    auto body = http_post(auth_url("signup"), j.dump(), {});
    if (body.empty()) {
        result.error = "Sign up failed - no response";
        return result;
    }

    try {
        auto resp = nlohmann::json::parse(body);
        if (resp.contains("id")) {
            result.success = true;
            result.access_token = resp.value("access_token", "");
            result.refresh_token = resp.value("refresh_token", "");
            SupabaseUser u;
            u.id = resp["id"];
            u.email = resp.value("email", email);
            if (resp.contains("user_metadata") && resp["user_metadata"].contains("display_name"))
                u.display_name = resp["user_metadata"]["display_name"];
            u.role = "user";
            result.user = u;
        } else {
            result.error = resp.value("msg", resp.value("error", "Unknown error"));
            if (resp.contains("message")) result.error = resp["message"];
        }
    } catch (const std::exception& e) {
        result.error = std::string("Parse error: ") + e.what();
    }
    return result;
}

SupabaseAuthResult SupabaseClient::sign_in(
    const std::string& email,
    const std::string& password)
{
    SupabaseAuthResult result;
    if (!configured_) { result.error = "Supabase not configured"; return result; }

    nlohmann::json j;
    j["email"] = email;
    j["password"] = password;

    auto body = http_post(auth_url("token?grant_type=password"), j.dump(), {});
    if (body.empty()) {
        result.error = "Sign in failed - no response";
        return result;
    }

    try {
        auto resp = nlohmann::json::parse(body);
        if (resp.contains("access_token")) {
            result.success = true;
            result.access_token = resp["access_token"];
            result.refresh_token = resp.value("refresh_token", "");
            SupabaseUser u;
            if (resp.contains("user")) {
                auto& user = resp["user"];
                u.id = user.value("id", "");
                u.email = user.value("email", email);
                if (user.contains("user_metadata") && user["user_metadata"].contains("display_name"))
                    u.display_name = user["user_metadata"]["display_name"];
            }
            u.role = "user";
            result.user = u;
        } else {
            result.error = resp.value("error_description",
                         resp.value("msg", resp.value("error", "Invalid credentials")));
        }
    } catch (const std::exception& e) {
        result.error = std::string("Parse error: ") + e.what();
    }
    return result;
}

SupabaseAuthResult SupabaseClient::sign_in_with_token(const std::string& refresh_token) {
    SupabaseAuthResult result;
    if (!configured_) { result.error = "Supabase not configured"; return result; }

    nlohmann::json j;
    j["refresh_token"] = refresh_token;

    auto body = http_post(auth_url("token?grant_type=refresh_token"), j.dump(), {});
    if (body.empty()) { result.error = "Token refresh failed"; return result; }

    try {
        auto resp = nlohmann::json::parse(body);
        if (resp.contains("access_token")) {
            result.success = true;
            result.access_token = resp["access_token"];
            result.refresh_token = resp.value("refresh_token", "");
        } else {
            result.error = resp.value("error_description", "Token refresh failed");
        }
    } catch (...) {
        result.error = "Parse error";
    }
    return result;
}

std::optional<SupabaseUser> SupabaseClient::get_user(const std::string& access_token) {
    if (!configured_ || access_token.empty()) return std::nullopt;

    auto body = http_get(auth_url("user"), {
        {"Authorization", "Bearer " + access_token}
    });

    if (body.empty()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(body);
        SupabaseUser u;
        u.id = j.value("id", "");
        u.email = j.value("email", "");
        if (j.contains("user_metadata") && j["user_metadata"].contains("display_name"))
            u.display_name = j["user_metadata"]["display_name"];
        u.role = j.value("role", "user");
        if (!u.id.empty()) return u;
    } catch (...) {}
    return std::nullopt;
}

std::optional<std::string> SupabaseClient::query(
    const std::string& table,
    const std::string& select,
    const std::string& filter,
    const std::string& access_token)
{
    if (!configured_) return std::nullopt;

    std::string url = rest_url(table) + "?select=" + select;
    if (!filter.empty()) url += "&" + filter;

    std::map<std::string, std::string> headers;
    if (!access_token.empty())
        headers["Authorization"] = "Bearer " + access_token;

    auto body = http_get(url, headers);
    if (body.empty()) return std::nullopt;
    return body;
}

bool SupabaseClient::insert(
    const std::string& table,
    const std::string& json_body,
    const std::string& access_token)
{
    if (!configured_) return false;

    std::map<std::string, std::string> headers;
    headers["Prefer"] = "return=minimal";
    if (!access_token.empty())
        headers["Authorization"] = "Bearer " + access_token;

    auto body = http_post(rest_url(table), json_body, headers);
    return !body.empty() || true;
}

bool SupabaseClient::update(
    const std::string& table,
    const std::string& json_body,
    const std::string& filter,
    const std::string& access_token)
{
    if (!configured_) return false;

    std::string url = rest_url(table) + "?" + filter;
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Prefer"] = "return=minimal";
    if (!access_token.empty())
        headers["Authorization"] = "Bearer " + access_token;

    auto resp = winhttp_request("PATCH", url, json_body, headers);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool SupabaseClient::remove(
    const std::string& table,
    const std::string& filter,
    const std::string& access_token)
{
    if (!configured_) return false;

    std::string url = rest_url(table) + "?" + filter;
    std::map<std::string, std::string> headers;
    if (!access_token.empty())
        headers["Authorization"] = "Bearer " + access_token;

    auto resp = winhttp_request("DELETE", url, "", headers);
    return resp.status_code >= 200 && resp.status_code < 300;
}

std::string SupabaseClient::user_id_from_token(const std::string& access_token) {
    auto first_dot = access_token.find('.');
    if (first_dot == std::string::npos) return "";
    auto second_dot = access_token.find('.', first_dot + 1);
    if (second_dot == std::string::npos) return "";

    std::string payload_b64 = access_token.substr(first_dot + 1, second_dot - first_dot - 1);

    std::string b64 = payload_b64;
    for (auto& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (b64.size() % 4) b64.push_back('=');

    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> decoded;
    decoded.reserve((b64.size() / 4) * 3);
    for (size_t i = 0; i < b64.size(); i += 4) {
        if (b64[i] == '=') break;
        unsigned val = (unsigned)chars.find(b64[i]) << 18;
        val |= (unsigned)chars.find(b64[i + 1]) << 12;
        val |= (i + 2 < b64.size() && b64[i + 2] != '=' ? (unsigned)chars.find(b64[i + 2]) << 6 : 0);
        val |= (i + 3 < b64.size() && b64[i + 3] != '=' ? (unsigned)chars.find(b64[i + 3]) : 0);
        decoded.push_back((val >> 16) & 0xFF);
        if (i + 2 < b64.size() && b64[i + 2] != '=') decoded.push_back((val >> 8) & 0xFF);
        if (i + 3 < b64.size() && b64[i + 3] != '=') decoded.push_back(val & 0xFF);
    }

    try {
        auto j = nlohmann::json::parse(std::string(decoded.begin(), decoded.end()));
        auto sub = j.value("sub", "");
        if (sub.empty()) sub = j.value("id", "");
        return sub;
    } catch (...) {
        return "";
    }
}

std::string SupabaseClient::username_to_email(const std::string& username) {
    if (username.find('@') != std::string::npos)
        return username;
    return username + "@gmail.com";
}
