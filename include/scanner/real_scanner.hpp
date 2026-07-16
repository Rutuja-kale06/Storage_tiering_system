#pragma once

#include "core/file_record.hpp"
#include "catalog/catalog_interface.hpp"
#include <string>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

struct ScanResult {
    size_t scanned  = 0;
    size_t added    = 0;
    size_t skipped  = 0;
    size_t errors   = 0;
    size_t max_reached = 0;
};

class RealScanner {
public:
    RealScanner();

    // Configure scan
    void set_root(const std::string& path);
    void set_recursive(bool recursive) { recursive_ = recursive; }
    void set_max_files(size_t max)     { max_files_ = max; }
    void set_min_size(uint64_t bytes)  { min_size_bytes_ = bytes; }
    void set_owner_id(const std::string& owner_id) { owner_id_ = owner_id; }
    void set_default_tier(Tier tier) { default_tier_ = tier; has_default_tier_ = true; }

    // Progress callback during scan
    using ProgressCallback = std::function<void(int scanned, const std::string& current_file)>;
    void set_progress_callback(ProgressCallback cb) { progress_cb_ = std::move(cb); }

    // Run the scan, populate catalog. Returns scan result.
    ScanResult scan(CatalogInterface& catalog, bool verbose = false);

    // Static: prompt user for directory path (interactive)
    static std::string prompt_directory();

    // Get the last scan result
    const ScanResult& last_result() const { return last_result_; }

    // Convert file_time_type to time_t
    static time_t to_time_t(fs::file_time_type ft);

private:
    std::string root_path_;
    bool        recursive_ = true;
    size_t      max_files_ = 5000;
    uint64_t    min_size_bytes_ = 0;
    std::string owner_id_;
    Tier        default_tier_ = Tier::HOT;
    bool        has_default_tier_ = false;
    ScanResult  last_result_;
    ProgressCallback progress_cb_;

    FileRecord build_record(const fs::directory_entry& entry,
                            const std::string& id,
                            time_t now) const;
};

// Top-level interactive function
void run_interactive_scan(CatalogInterface& catalog);
