#include "migration/local_migrator.hpp"
#include "core/types.hpp"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace fs = std::filesystem;

LocalMigrator::LocalMigrator(const std::string& base_directory)
    : base_dir_(base_directory) {}

bool LocalMigrator::create_tier_directories() {
    bool ok = true;
    for (int i = 0; i < TIER_COUNT; ++i) {
        fs::path p = fs::path(base_dir_) / TIER_NAMES[i];
        try {
            fs::create_directories(p);
        } catch (...) { ok = false; }
    }
    return ok;
}

std::string LocalMigrator::tier_directory(Tier t) const {
    return (fs::path(base_dir_) / TIER_NAMES[static_cast<int>(t)]).string();
}

std::string LocalMigrator::unique_path(const std::string& dir, const std::string& filename) const {
    fs::path dst = fs::path(dir) / filename;
    if (!fs::exists(dst)) return dst.string();

    // Append counter to avoid overwrite
    fs::path stem = filename;
    std::string ext = stem.extension().string();
    std::string base = stem.stem().string();
    for (int n = 1; n < 1000; ++n) {
        dst = fs::path(dir) / (base + "_" + std::to_string(n) + ext);
        if (!fs::exists(dst)) return dst.string();
    }
    return "";  // too many collisions
}

bool LocalMigrator::can_migrate(const FileRecord& file, Tier target_tier) {
    // Check source exists
    if (!fs::exists(file.path)) return false;

    // Check target directory exists or can be created
    fs::path target_dir = fs::path(base_dir_) / TIER_NAMES[static_cast<int>(target_tier)];
    try {
        fs::create_directories(target_dir);
        return true;
    } catch (...) {
        return false;
    }
}

MigrateResult LocalMigrator::migrate(FileRecord& file, Tier target_tier) {
    MigrateResult result;
    auto start = std::chrono::steady_clock::now();

    if (!can_migrate(file, target_tier)) {
        result.error_message = "Pre-flight check failed";
        return result;
    }

    // Create backup record
    BackupRecord backup;
    backup.original_path = file.path;
    backup.original_tier = file.current_tier;
    backup.timestamp = std::time(nullptr);

    // Generate backup path
    std::string backup_dir = (fs::path(base_dir_) / ".backups").string();
    fs::create_directories(backup_dir);
    std::string stem = fs::path(file.path).stem().string();
    std::string ext  = fs::path(file.path).extension().string();
    auto now = std::time(nullptr);
    struct tm* tm_info = std::localtime(&now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm_info);
    backup.backup_path = (fs::path(backup_dir) / (stem + "_" + ts + ext)).string();

    try {
        // Create backup
        if (fs::exists(file.path)) {
            fs::copy_file(file.path, backup.backup_path, fs::copy_options::overwrite_existing);
        }

        // Destination path
        std::string filename = fs::path(file.path).filename().string();
        std::string dst_path = unique_path(tier_directory(target_tier), filename);
        if (dst_path.empty()) {
            result.error_message = "Could not generate unique target path";
            return result;
        }

        // Move file
        fs::rename(file.path, dst_path);

        // Verify
        if (fs::exists(dst_path) && fs::file_size(dst_path) == file.size_bytes) {
            file.path = dst_path;
            file.current_tier = target_tier;
            file.migrate_count++;
            result.success = true;
            result.new_path = dst_path;

            backups_[file.id] = backup;
            cleanup_old_backups();
        } else {
            // Verification failed: restore from backup
            if (fs::exists(backup.backup_path)) {
                fs::copy_file(backup.backup_path, file.path, fs::copy_options::overwrite_existing);
            }
            result.error_message = "Verification failed after migration";
        }
    } catch (const fs::filesystem_error& e) {
        // Cross-device move: fall back to copy+delete
        try {
            std::string filename = fs::path(file.path).filename().string();
            std::string dst_path = unique_path(tier_directory(target_tier), filename);
            fs::copy_file(file.path, dst_path, fs::copy_options::overwrite_existing);
            fs::remove(file.path);

            if (fs::exists(dst_path) && fs::file_size(dst_path) == file.size_bytes) {
                file.path = dst_path;
                file.current_tier = target_tier;
                file.migrate_count++;
                result.success = true;
                result.new_path = dst_path;
                backups_[file.id] = backup;
                cleanup_old_backups();
            }
        } catch (const std::exception& ex) {
            result.error_message = ex.what();
        }
    } catch (const std::exception& e) {
        result.error_message = e.what();
    }

    auto end = std::chrono::steady_clock::now();
    result.duration_ms = std::chrono::duration<double, std::milli>(end - start).count();
    return result;
}

bool LocalMigrator::rollback(FileRecord& file, const std::string& original_path) {
    auto it = backups_.find(file.id);
    if (it == backups_.end()) return false;

    try {
        // If file exists at current location, move it back
        if (fs::exists(file.path)) {
            fs::rename(file.path, original_path);
        } else if (fs::exists(it->second.backup_path)) {
            // Restore from backup
            fs::copy_file(it->second.backup_path, original_path, fs::copy_options::overwrite_existing);
        } else {
            return false;
        }

        file.path = original_path;
        file.current_tier = it->second.original_tier;
        file.migrate_count--;
        backups_.erase(it);
        return true;
    } catch (...) {
        return false;
    }
}

void LocalMigrator::cleanup_old_backups() {
    if (!fs::exists(fs::path(base_dir_) / ".backups")) return;

    time_t now = std::time(nullptr);
    std::vector<fs::directory_entry> backups_list;
    auto backup_dir = fs::path(base_dir_) / ".backups";

    for (auto& entry : fs::directory_iterator(backup_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ft = entry.last_write_time();
        auto fp_time = std::chrono::file_clock::to_sys(ft);
        auto age = std::difftime(now, std::chrono::system_clock::to_time_t(fp_time));
        if (age > max_backup_age_days_ * 86400.0) {
            fs::remove(entry.path());
            continue;
        }
        backups_list.push_back(entry);
    }

    // Enforce count limit (remove oldest first)
    if (static_cast<int>(backups_list.size()) > max_backup_count_) {
        std::sort(backups_list.begin(), backups_list.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) {
                return a.last_write_time() < b.last_write_time();
            });
        int to_remove = static_cast<int>(backups_list.size()) - max_backup_count_;
        for (int i = 0; i < to_remove; ++i)
            fs::remove(backups_list[i].path());
    }
}

bool LocalMigrator::verify(const FileRecord& file) {
    try {
        return fs::exists(file.path) &&
               fs::is_regular_file(file.path) &&
               fs::file_size(file.path) == file.size_bytes;
    } catch (...) {
        return false;
    }
}
