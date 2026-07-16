#include "monitoring/file_watcher.hpp"
#include "logger.hpp"
#include <filesystem>
#include <chrono>
#include <thread>
#include <mutex>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

struct FileWatcher::Impl {
#ifdef _WIN32
    struct WatchHandle {
        std::string directory;
        HANDLE      dir_handle;
        OVERLAPPED  overlapped;
        char        buffer[65536];
    };
    std::vector<WatchHandle> handles_;
    void poll_windows(FileWatcher& self);
#else
    int inotify_fd_ = -1;
    std::map<int, std::string> watch_descriptors_;
    void poll_inotify(FileWatcher& self);
#endif
};

FileWatcher::FileWatcher() : impl_(std::make_unique<Impl>()) {}

FileWatcher::~FileWatcher() {
    stop();
}

bool FileWatcher::watch(const std::string& directory, bool recursive) {
    if (!fs::exists(directory) || !fs::is_directory(directory))
        return false;

    watch_dirs_.push_back(directory);
    recursive_ = recursive;

    LOG_INFO("FileWatcher", "Watching directory: " + directory);
    return true;
}

bool FileWatcher::unwatch(const std::string& directory) {
    auto it = std::find(watch_dirs_.begin(), watch_dirs_.end(), directory);
    if (it != watch_dirs_.end()) {
        watch_dirs_.erase(it);
        {
            std::lock_guard<std::mutex> lk(cache_mtx_);
            file_cache_.erase(directory);
        }
        return true;
    }
    return false;
}

void FileWatcher::unwatch_all() {
    watch_dirs_.clear();
    {
        std::lock_guard<std::mutex> lk(cache_mtx_);
        file_cache_.clear();
    }
}

bool FileWatcher::start() {
    if (running_.load()) return false;

    running_.store(true);
    worker_ = std::thread([this]() {
        LOG_INFO("FileWatcher", "Polling-based file watcher started (interval="
                 + std::to_string(poll_interval_ms_) + "ms)");

        while (running_.load()) {
            poll_filesystem();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(poll_interval_ms_));
        }
    });
    worker_.detach();

    return true;
}

void FileWatcher::stop() {
    running_.store(false);
    if (worker_.joinable())
        worker_.join();
}

void FileWatcher::poll_filesystem() {
    std::map<std::string, std::map<std::string, fs::file_time_type>> snapshot;
    {
        std::lock_guard<std::mutex> lk(cache_mtx_);
        snapshot = file_cache_;
    }
    for (const auto& [dir, old_cache] : snapshot) {
        std::map<std::string, fs::file_time_type> new_cache;

        try {
            auto poll_dir = [&](auto&& iter) {
                for (auto& entry : iter) {
                    if (!entry.is_regular_file()) continue;

                    std::string path = entry.path().string();
                    auto mtime = entry.last_write_time();
                    new_cache[path] = mtime;

                    auto old_it = old_cache.find(path);
                    if (old_it == old_cache.end()) {
                        LOG_DEBUG("FileWatcher", "Created: " + path);
                        if (on_created_) on_created_(path);
                    } else if (old_it->second != mtime) {
                        LOG_DEBUG("FileWatcher", "Modified: " + path);
                        if (on_modified_) on_modified_(path);
                    }
                }
            };
            if (recursive_)
                poll_dir(fs::recursive_directory_iterator(dir,
                    fs::directory_options::skip_permission_denied));
            else
                poll_dir(fs::directory_iterator(dir));

            // Check for deleted files
            for (const auto& [path, _] : old_cache) {
                if (new_cache.find(path) == new_cache.end()) {
                    LOG_DEBUG("FileWatcher", "Deleted: " + path);
                    if (on_deleted_) on_deleted_(path);
                }
            }

            {
                std::lock_guard<std::mutex> lk(cache_mtx_);
                file_cache_[dir] = new_cache;
            }

        } catch (const fs::filesystem_error& e) {
            LOG_WARN("FileWatcher", std::string("Error polling: ") + e.what());
        }
    }
}
