#include "api/fileserver.hpp"
#include "logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

static const std::vector<std::string> search_paths = {
    "./fileserver.html", "../src/api/fileserver.html",
    "src/api/fileserver.html", "../fileserver.html"
};

static std::string mime_type(const std::string& ext) {
    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "css") return "text/css";
    if (ext == "js")  return "application/javascript";
    if (ext == "json") return "application/json";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "webp") return "image/webp";
    if (ext == "ico") return "image/x-icon";
    if (ext == "pdf") return "application/pdf";
    if (ext == "zip") return "application/zip";
    if (ext == "gz")  return "application/gzip";
    if (ext == "tar") return "application/x-tar";
    if (ext == "mp4") return "video/mp4";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "wav") return "audio/wav";
    if (ext == "txt" || ext == "md" || ext == "csv" || ext == "log") return "text/plain";
    if (ext == "yaml" || ext == "yml") return "text/yaml";
    if (ext == "xml") return "text/xml";
    return "application/octet-stream";
}

FileServer::FileServer(const std::string& base_path) : base_path_(base_path) {}

void FileServer::set_user(const std::string& username, bool is_admin) {
    current_user_ = is_admin ? "" : username;
}

bool FileServer::resolve(const std::string& virtual_path, std::string& real_path) const {
    LOG_INFO("FileServer", "resolve called: virtual_path='" + virtual_path + "', current_user_='" + current_user_ + "', base_path_='" + base_path_ + "'");
    fs::path resolved = fs::absolute(base_path_);
    if (!current_user_.empty())
        resolved = resolved / current_user_;
    if (virtual_path.empty() || virtual_path == "/") {
        real_path = resolved.string();
        LOG_INFO("FileServer", "resolve returning root: real_path='" + real_path + "'");
        return true;
    }
    std::string clean = virtual_path;
    if (clean.front() == '/') clean.erase(0, 1);
    resolved = fs::weakly_canonical(resolved / clean);
    auto base_str = fs::absolute(base_path_).string();
    if (!current_user_.empty())
        base_str = (fs::absolute(base_path_) / current_user_).string();
    auto res_str = resolved.string();
    if (res_str.find(base_str) != 0) {
        LOG_WARN("FileServer", "Path traversal blocked: " + virtual_path);
        return false;
    }
    real_path = res_str;
    return true;
}

std::string FileServer::handle_list(const std::string& path_str, std::string& out_path) const {
    try {
        std::string real;
        if (!resolve(path_str, real)) {
            nlohmann::json err;
            err["error"] = "Invalid path";
            return err.dump();
        }
        out_path = path_str.empty() ? "/" : path_str;
        LOG_INFO("FileServer", "handle_list: real='" + real + "', exists=" + (fs::exists(real) ? "true" : "false"));
        if (!fs::exists(real)) {
            nlohmann::json err;
            err["error"] = "Path does not exist: " + real;
            return err.dump();
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& entry : fs::directory_iterator(real)) {
            auto filename = entry.path().filename().string();
            if (filename.front() == '.') continue;
            nlohmann::json j;
            j["name"] = filename;
            j["is_dir"] = entry.is_directory();
            j["size"] = entry.is_regular_file() ? (int64_t)entry.file_size() : 0;
            auto ft = entry.last_write_time();
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
            std::stringstream ss;
            ss << std::put_time(std::localtime(&tt), "%Y-%m-%d %H:%M:%S");
            j["modified"] = ss.str();
            arr.push_back(j);
        }
        nlohmann::json resp;
        resp["files"] = arr;
        resp["path"] = out_path;
        return resp.dump();
    } catch (const std::exception& e) {
        LOG_ERROR("FileServer", "list error: " + std::string(e.what()));
        nlohmann::json err;
        err["error"] = e.what();
        return err.dump();
    }
}

bool FileServer::handle_upload(const std::string& path_str, const std::string& data) const {
    try {
        std::string real;
        if (!resolve(path_str, real)) return false;
        // Ensure parent directory exists
        fs::create_directories(fs::path(real).parent_path());
        std::ofstream ofs(real, std::ios::binary);
        if (!ofs) return false;
        ofs.write(data.data(), data.size());
        return ofs.good();
    } catch (const std::exception& e) {
        LOG_ERROR("FileServer", "upload error: " + std::string(e.what()));
        return false;
    }
}

bool FileServer::handle_download(const std::string& path_str, std::string& out_data, std::string& out_mime) const {
    try {
        std::string real;
        if (!resolve(path_str, real)) return false;
        std::ifstream ifs(real, std::ios::binary | std::ios::ate);
        if (!ifs) return false;
        auto size = ifs.tellg();
        ifs.seekg(0);
        out_data.resize(static_cast<size_t>(size));
        ifs.read(out_data.data(), size);
        auto ext = fs::path(real).extension().string();
        if (!ext.empty()) ext = ext.substr(1);
        out_mime = mime_type(ext);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("FileServer", "download error: " + std::string(e.what()));
        return false;
    }
}

bool FileServer::handle_delete(const std::string& path_str) const {
    try {
        std::string real;
        if (!resolve(path_str, real)) return false;
        if (!fs::exists(real)) return false;
        fs::remove_all(real);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("FileServer", "delete error: " + std::string(e.what()));
        return false;
    }
}

bool FileServer::handle_mkdir(const std::string& path_str) const {
    try {
        std::string real;
        if (!resolve(path_str, real)) return false;
        return fs::create_directories(real);
    } catch (const std::exception& e) {
        LOG_ERROR("FileServer", "mkdir error: " + std::string(e.what()));
        return false;
    }
}

bool FileServer::handle_rename(const std::string& old_path_str, const std::string& new_path_str) const {
    try {
        std::string old_real, new_real;
        if (!resolve(old_path_str, old_real)) return false;
        if (!resolve(new_path_str, new_real)) return false;
        fs::rename(old_real, new_real);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("FileServer", "rename error: " + std::string(e.what()));
        return false;
    }
}

std::string FileServer::handle_stats() const {
    try {
        nlohmann::json stats;
        std::error_code ec;
        auto space = fs::space(base_path_, ec);
        if (!ec) {
            stats["total_bytes"] = (int64_t)space.capacity;
            stats["used_bytes"] = (int64_t)(space.capacity - space.available);
            stats["free_bytes"] = (int64_t)space.available;
        }
        // Count files and dirs
        size_t file_count = 0, dir_count = 0;
        if (fs::exists(base_path_)) {
            for (auto& it : fs::recursive_directory_iterator(base_path_, ec)) {
                if (it.is_regular_file()) ++file_count;
                else if (it.is_directory()) ++dir_count;
            }
        }
        stats["file_count"] = (int64_t)file_count;
        stats["dir_count"] = (int64_t)dir_count;
        stats["base_path"] = base_path_;
        return stats.dump();
    } catch (const std::exception& e) {
        LOG_ERROR("FileServer", "stats error: " + std::string(e.what()));
        return "{}";
    }
}

std::string FileServer::html_content() {
    for (const auto& path : search_paths) {
        std::ifstream ifs(path);
        if (ifs.is_open()) {
            std::stringstream buf;
            buf << ifs.rdbuf();
            auto html = buf.str();
            if (!html.empty()) {
                LOG_DEBUG("FileServer", "Loaded HTML from " + path);
                return html;
            }
        }
    }
    LOG_WARN("FileServer", "fileserver.html not found");
    return "<html><body><h1>FileServer not available</h1></body></html>";
}
