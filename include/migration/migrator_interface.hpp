#pragma once

#include "core/file_record.hpp"
#include <string>
#include <optional>

enum class MigratorType {
    LOCAL,
    S3,
    AZURE_BLOB,
    GCS,
    SMB
};

struct MigrateResult {
    bool        success = false;
    std::string new_path;
    std::string error_message;
    double      duration_ms = 0.0;
};

class Migrator {
public:
    virtual ~Migrator() = default;
    virtual MigratorType type() const = 0;
    virtual std::string name() const = 0;

    // Migrate a file to the target tier. Returns new path on success.
    virtual MigrateResult migrate(FileRecord& file, Tier target_tier) = 0;

    // Rollback a migration (restore to original location/tier)
    virtual bool rollback(FileRecord& file, const std::string& original_path) = 0;

    // Verify file integrity after migration
    virtual bool verify(const FileRecord& file) = 0;

    // Pre-flight check: can we migrate this file?
    virtual bool can_migrate(const FileRecord& file, Tier target_tier) = 0;
};
