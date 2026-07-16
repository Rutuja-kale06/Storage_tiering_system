#include "scanner/real_scanner.hpp"
#include "core/types.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>

using namespace std::chrono;

RealScanner::RealScanner() = default;

time_t RealScanner::to_time_t(fs::file_time_type ft) {
    auto sctp = time_point_cast<system_clock::duration>(
        ft - fs::file_time_type::clock::now() + system_clock::now());
    return system_clock::to_time_t(sctp);
}

void RealScanner::set_root(const std::string& path) {
    root_path_ = path;
}

FileRecord RealScanner::build_record(const fs::directory_entry& entry,
                                      const std::string& id,
                                      time_t now) const
{
    FileRecord f;
    f.id        = id;
    f.path      = entry.path().string();
    f.extension = entry.path().extension().string();

    // Lowercase extension
    std::transform(f.extension.begin(), f.extension.end(),
                   f.extension.begin(), ::tolower);

    f.file_type      = classify_extension(f.extension);
    f.size_bytes     = static_cast<int64_t>(entry.file_size());
    f.access_count   = 0;
    f.write_count    = 0;

    time_t mtime = to_time_t(entry.last_write_time());
    f.created_at     = mtime;
    f.last_modified  = mtime;
    f.last_accessed  = mtime;

    // Assign initial tier based on age, or use default tier if set
    if (has_default_tier_) {
        f.current_tier = default_tier_;
    } else {
        double idle_days = difftime(now, mtime) / 86400.0;
        if      (idle_days < 7.0)   f.current_tier = Tier::HOT;
        else if (idle_days < 30.0)  f.current_tier = Tier::WARM;
        else if (idle_days < 180.0) f.current_tier = Tier::COLD;
        else                        f.current_tier = Tier::ARCHIVE;
    }

    f.target_tier  = f.current_tier;
    f.migrate_count = 0;
    f.is_pinned     = false;
    f.is_critical   = (f.file_type == FileType::DATABASE);
    f.score         = 0.0;
    f.owner_id      = owner_id_;

    return f;
}

ScanResult RealScanner::scan(CatalogInterface& catalog, bool verbose) {
    ScanResult result;
    fs::path root(root_path_);

    if (!fs::exists(root)) {
        if (verbose)
            std::cout << RED << "  [X] Path does not exist: "
                      << root_path_ << RESET << "\n";
        return result;
    }
    if (!fs::is_directory(root)) {
        if (verbose)
            std::cout << RED << "  [X] Not a directory: "
                       << root_path_ << RESET << "\n";
        return result;
    }

    time_t now = time(nullptr);
    int id_counter = 0;

    try {
        auto scan_impl = [&](auto&& iterator) {
            for (auto& entry : iterator) {
                if (result.scanned >= max_files_) {
                    result.max_reached = max_files_;
                    break;
                }

                result.scanned++;

                try {
                    if (!entry.is_regular_file()) {
                        result.skipped++;
                        continue;
                    }

                    uint64_t size = entry.file_size();
                    if (size < min_size_bytes_) {
                        result.skipped++;
                        continue;
                    }

                    std::ostringstream idss;
                    idss << "real_" << std::setw(6) << std::setfill('0') << id_counter++;
                    FileRecord f = build_record(entry, idss.str(), now);

                    // Check for duplicates by path
                    auto existing = catalog.get_file_by_path(f.path);
                    if (existing.has_value()) {
                        result.skipped++;
                        continue;
                    }

                    catalog.add_file(f);
                    result.added++;

                    if (progress_cb_)
                        progress_cb_(static_cast<int>(result.added), f.path);

                } catch (...) {
                    result.errors++;
                }
            }
        };

        if (recursive_)
            scan_impl(fs::recursive_directory_iterator(
                root, fs::directory_options::skip_permission_denied));
        else
            scan_impl(fs::directory_iterator(root));
    } catch (const fs::filesystem_error& e) {
        if (verbose)
            std::cout << YELLOW << "  Warning: " << e.what() << RESET << "\n";
    }

    last_result_ = result;
    return result;
}

std::string RealScanner::prompt_directory() {
    std::cout << "\n  Enter directory path to scan\n";
    std::cout << "  (e.g. C:\\Users\\YourName\\Documents or /home/user/data)\n";
    std::cout << "  Path: ";
    std::string path;
    std::getline(std::cin, path);
    while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    return path;
}

void run_interactive_scan(CatalogInterface& catalog) {
    std::cout << "\n" << BOLD << "  REAL FILESYSTEM SCANNER\n" << RESET;
    std::cout << "  " << std::string(60, '-') << "\n";

    std::string dir = RealScanner::prompt_directory();
    if (dir.empty()) {
        std::cout << RED << "  No path entered.\n" << RESET;
        return;
    }

    std::cout << "  Recursive scan? (y/n) [y]: ";
    char rec; std::cin >> rec;
    bool recursive = (rec != 'n' && rec != 'N');

    std::cout << "  Max files [1000]: ";
    std::string max_str; std::cin >> max_str;
    size_t max_files = 1000;
    try { max_files = std::stoul(max_str); } catch (...) {}

    RealScanner scanner;
    scanner.set_root(dir);
    scanner.set_recursive(recursive);
    scanner.set_max_files(max_files);

    auto result = scanner.scan(catalog, true);

    std::cout << "\n  " << GREEN << "Scan complete: " << result.added
              << " files added (" << result.skipped << " skipped, "
              << result.errors << " errors)\n" << RESET;
}
