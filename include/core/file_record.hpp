#pragma once

#include "tier.hpp"
#include <string>
#include <cstdint>
#include <ctime>
#include <cmath>

enum class FileType : int {
    DATABASE    = 0,
    ANALYTICS   = 1,
    LOGS        = 2,
    MEDIA       = 3,
    ARCHIVE_ZIP = 4,
    BACKUP      = 5,
    EXECUTABLE  = 6,
    TEMPORARY   = 7,
    CONFIG      = 8,
    OTHER       = 9
};

constexpr int FILE_TYPE_COUNT = 10;

inline constexpr const char* FILE_TYPE_NAMES[] = {
    "Database", "Analytics", "Log", "Media",
    "Archive", "Backup", "Executable",
    "Temporary", "Config", "Other"
};

inline std::string file_type_name(FileType ft) {
    int idx = static_cast<int>(ft);
    return (idx >= 0 && idx < FILE_TYPE_COUNT) ? FILE_TYPE_NAMES[idx] : "Other";
}

inline FileType classify_extension(const std::string& ext) {
    if (ext == ".db" || ext == ".sqlite" || ext == ".mdb" || ext == ".sqlite3")
        return FileType::DATABASE;
    if (ext == ".csv" || ext == ".parquet" || ext == ".json" || ext == ".xlsx" || ext == ".avro")
        return FileType::ANALYTICS;
    if (ext == ".log" || ext == ".out")
        return FileType::LOGS;
    if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mp3" || ext == ".flac")
        return FileType::MEDIA;
    if (ext == ".zip" || ext == ".gz" || ext == ".7z" || ext == ".tar" || ext == ".tar.gz")
        return FileType::ARCHIVE_ZIP;
    if (ext == ".bak" || ext == ".dump" || ext == ".backup")
        return FileType::BACKUP;
    if (ext == ".exe" || ext == ".so" || ext == ".dll" || ext == ".bin")
        return FileType::EXECUTABLE;
    if (ext == ".tmp" || ext == ".swp" || ext == ".lock")
        return FileType::TEMPORARY;
    if (ext == ".conf" || ext == ".cfg" || ext == ".ini" || ext == ".yaml" || ext == ".yml")
        return FileType::CONFIG;
    return FileType::OTHER;
}

struct FileRecord {
    std::string  id;
    std::string  path;
    std::string  extension;
    FileType     file_type;
    Tier         current_tier;
    Tier         target_tier;

    int64_t      size_bytes;
    int32_t      access_count;
    int32_t      write_count;
    time_t       created_at;
    time_t       last_accessed;
    time_t       last_modified;
    int32_t      migrate_count;

    bool         is_pinned;
    bool         is_critical;
    double       score;

    // Ownership
    std::string  owner_id;    // empty = system-owned

    // S3-specific metadata
    std::string  s3_bucket;
    std::string  s3_key;
    std::string  content_type;
    std::string  etag;

    double size_mb()  const { return size_bytes / (1024.0 * 1024.0); }
    double size_gb()  const { return size_bytes / (1024.0 * 1024.0 * 1024.0); }

    double idle_days() const {
        return std::difftime(std::time(nullptr), last_accessed) / 86400.0;
    }

    double age_days() const {
        return std::difftime(std::time(nullptr), created_at) / 86400.0;
    }

    double access_rate() const {
        double age = std::max(age_days(), 0.001);
        return access_count / age;
    }

    double monthly_cost() const {
        return size_gb() * tier_cost_per_gb(current_tier) * 30.0;
    }
};

struct MigrationEvent {
    std::string file_id;
    std::string file_path;
    Tier        from_tier;
    Tier        to_tier;
    int64_t     size_bytes;
    std::string reason;
    time_t      timestamp;
    bool        success;
    double      duration_ms;

    std::string time_str() const {
        char buf[32];
        struct tm* tm_info = std::localtime(&timestamp);
        std::strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
        return std::string(buf);
    }
};

struct TierStats {
    Tier     tier;
    int      file_count  = 0;
    int64_t  total_bytes = 0;
    double   monthly_cost_usd = 0.0;
    int      total_accesses   = 0;

    double total_gb() const { return total_bytes / (1024.0*1024.0*1024.0); }
};
