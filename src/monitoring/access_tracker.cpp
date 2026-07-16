#include "monitoring/access_tracker.hpp"
#include "catalog/catalog_interface.hpp"
#include "logger.hpp"
#include <filesystem>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;

AccessTracker::AccessTracker() = default;

AccessTracker::~AccessTracker() {
    stop();
}

bool AccessTracker::start(const std::string& root_path) {
    if (running_.load()) return false;

    root_path_ = root_path;
    running_.store(true);

    // Use polling fallback (works cross-platform without privileges)
    worker_ = std::thread([this]() {
        LOG_INFO("AccessTracker",
                 "Polling-based access tracker started for: " + root_path_);

        while (running_.load()) {
            if (fallback_enabled_)
                poll_access_times();
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    });
    worker_.detach();

    return true;
}

void AccessTracker::stop() {
    running_.store(false);
    if (worker_.joinable())
        worker_.join();
}

void AccessTracker::poll_access_times() {
    if (!catalog_) return;

    try {
        auto iter = fs::recursive_directory_iterator(
            root_path_, fs::directory_options::skip_permission_denied);

        for (auto& entry : iter) {
            if (!entry.is_regular_file()) continue;

            std::string path = entry.path().string();
            auto last_write = fs::last_write_time(entry.path());
            time_t mtime = std::chrono::system_clock::to_time_t(
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    last_write - fs::file_time_type::clock::now() +
                    std::chrono::system_clock::now()));

            auto it = last_atime_cache_.find(path);
            if (it != last_atime_cache_.end() && it->second != mtime) {
                // File was modified/accessed
                auto file_opt = catalog_->get_file_by_path(path);
                if (file_opt.has_value()) {
                    catalog_->record_access(file_opt->id);
                    events_tracked_.fetch_add(1);
                }
            }
            last_atime_cache_[path] = mtime;
        }
    } catch (const fs::filesystem_error& e) {
        LOG_WARN("AccessTracker", std::string("Poll error: ") + e.what());
    }
}
