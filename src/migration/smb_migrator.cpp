#include "migration/smb_migrator.hpp"
#include "core/types.hpp"
#include "logger.hpp"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winnetwk.h>
#endif

namespace fs = std::filesystem;

SmbMigrator::SmbMigrator(const SmbShareConfig& config, const std::string& tier_base)
    : config_(config)
    , label_(config.label.empty() ? config.host + "_" + config.share : config.label)
    , base_dir_(tier_base)
{
    // Build UNC path \\host\share
    share_unc_ = "\\\\" + config.host + "\\" + config.share;
}

bool SmbMigrator::mount_share() {
#ifdef _WIN32
    if (connected_) return true;

    NETRESOURCEA nr = {0};
    nr.dwType = RESOURCETYPE_DISK;
    nr.lpLocalName = const_cast<LPSTR>(config_.mount_point.empty() ? nullptr : config_.mount_point.c_str());
    nr.lpRemoteName = const_cast<LPSTR>(share_unc_.c_str());
    nr.lpProvider = nullptr;

    DWORD flags = CONNECT_TEMPORARY;
    std::string username;
    std::string password;

    if (!config_.domain.empty())
        username = config_.domain + "\\" + config_.username;
    else
        username = config_.username;

    DWORD ret = WNetAddConnection2A(&nr,
        config_.password.empty() ? nullptr : config_.password.c_str(),
        username.empty() ? nullptr : username.c_str(),
        flags);

    if (ret == NO_ERROR || ret == ERROR_ALREADY_ASSIGNED || ret == ERROR_DEVICE_ALREADY_REMEMBERED) {
        connected_ = true;
        LOG_INFO("SmbMigrator", "Mounted " + share_unc_ + " as tier " + tier_name(static_cast<Tier>(config_.tier)));
        return true;
    }

    LOG_ERROR("SmbMigrator", "Failed to mount " + share_unc_ + " error=" + std::to_string(ret));
    return false;
#else
    LOG_WARN("SmbMigrator", "SMB mounting not implemented on non-Windows");
    connected_ = true; // optimistic — assume already mounted
    return true;
#endif
}

bool SmbMigrator::unmount_share() {
    if (!connected_) return true;
#ifdef _WIN32
    DWORD ret = WNetCancelConnection2A(
        config_.mount_point.empty() ? share_unc_.c_str() : config_.mount_point.c_str(),
        0, TRUE);
    connected_ = false;
    return ret == NO_ERROR || ret == ERROR_NOT_CONNECTED || ret == ERROR_CONNECTION_UNAVAIL;
#else
    connected_ = false;
    return true;
#endif
}

bool SmbMigrator::is_connected() const {
#ifdef _WIN32
    if (!connected_) return false;
    // Quick check — try to access the UNC root
    return fs::exists(share_unc_);
#else
    return connected_ && fs::exists(share_unc_);
#endif
}

bool SmbMigrator::try_reconnect() {
    unmount_share();
    for (int i = 0; i < retry_count_; ++i) {
        if (mount_share()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms_ * (i + 1)));
    }
    return false;
}

std::string SmbMigrator::unc_path() const {
    return share_unc_;
}

std::string SmbMigrator::tier_subdir(Tier t) const {
    return (fs::path(config_.mount_point.empty() ? share_unc_ : config_.mount_point) / TIER_NAMES[static_cast<int>(t)]).string();
}

bool SmbMigrator::ensure_tier_dir(Tier t) {
#ifdef _WIN32
    if (!is_connected() && !try_reconnect()) return false;
#endif
    try {
        fs::create_directories(tier_subdir(t));
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("SmbMigrator", std::string("Failed to create tier dir: ") + e.what());
        return false;
    }
}

std::string SmbMigrator::unique_path(const std::string& dir, const std::string& filename) const {
    fs::path dst = fs::path(dir) / filename;
    if (!fs::exists(dst)) return dst.string();
    fs::path stem = filename;
    std::string ext = stem.extension().string();
    std::string base = stem.stem().string();
    for (int n = 1; n < 1000; ++n) {
        dst = fs::path(dir) / (base + "_" + std::to_string(n) + ext);
        if (!fs::exists(dst)) return dst.string();
    }
    return "";
}

bool SmbMigrator::can_migrate(const FileRecord& file, Tier target_tier) {
    // Only handle migrations TO this SMB tier
    if (static_cast<int>(target_tier) != config_.tier) return false;

    // Check source exists (could be local or another share)
    if (!fs::exists(file.path)) return false;

#ifdef _WIN32
    if (!is_connected() && !try_reconnect()) return false;
#endif

    // Check target dir exists or can be created
    return ensure_tier_dir(target_tier);
}

MigrateResult SmbMigrator::migrate(FileRecord& file, Tier target_tier) {
    MigrateResult result;
    auto start = std::chrono::steady_clock::now();

    if (!can_migrate(file, target_tier)) {
        result.error_message = "Cannot migrate to SMB share";
        return result;
    }

#ifdef _WIN32
    if (!is_connected() && !try_reconnect()) {
        result.error_message = "SMB share not reachable";
        return result;
    }
#endif

    try {
        std::string filename = fs::path(file.path).filename().string();
        std::string dst_path = unique_path(tier_subdir(target_tier), filename);
        if (dst_path.empty()) {
            result.error_message = "Could not generate unique target path";
            return result;
        }

        // Copy to SMB share — fail if destination already exists (safety)
        fs::copy_file(file.path, dst_path, fs::copy_options::none);

        // Verify
        if (fs::exists(dst_path) && fs::file_size(dst_path) == file.size_bytes) {
            // Remove local/original file
            std::error_code ec;
            fs::remove(file.path, ec);
            if (ec) {
                LOG_WARN("SmbMigrator", "Could not remove original " + file.path + ": " + ec.message());
            }

            file.path = dst_path;
            file.current_tier = target_tier;
            file.migrate_count++;
            result.success = true;
            result.new_path = dst_path;
        } else {
            fs::remove(dst_path);
            result.error_message = "Verification failed after SMB copy";
        }
    } catch (const fs::filesystem_error& e) {
        result.error_message = std::string("SMB filesystem error: ") + e.what();
    } catch (const std::exception& e) {
        result.error_message = std::string("SMB migration error: ") + e.what();
    }

    auto end = std::chrono::steady_clock::now();
    result.duration_ms = std::chrono::duration<double, std::milli>(end - start).count();
    return result;
}

bool SmbMigrator::rollback(FileRecord& file, const std::string& original_path) {
    try {
        if (fs::exists(file.path)) {
            fs::copy_file(file.path, original_path, fs::copy_options::overwrite_existing);
            // Verify rollback integrity
            if (!fs::exists(original_path) || fs::file_size(original_path) != file.size_bytes) {
                LOG_ERROR("SmbMigrator", "Rollback verification failed for " + original_path);
                return false;
            }
            fs::remove(file.path);
        } else {
            return false;
        }
        file.path = original_path;
        file.current_tier = Tier::HOT;
        file.migrate_count--;
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("SmbMigrator", std::string("Rollback failed: ") + e.what());
        return false;
    }
}

bool SmbMigrator::verify(const FileRecord& file) {
    try {
#ifdef _WIN32
        if (!is_connected() && !try_reconnect()) return false;
#endif
        return fs::exists(file.path) &&
               fs::is_regular_file(file.path) &&
               fs::file_size(file.path) == file.size_bytes;
    } catch (const std::exception& e) {
        LOG_ERROR("SmbMigrator", std::string("Verify failed: ") + e.what());
        return false;
    }
}
