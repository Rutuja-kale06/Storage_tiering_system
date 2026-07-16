#pragma once

#include "core/types.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <map>
#include <memory>
#include <filesystem>

class FileWatcher {
public:
    FileWatcher();
    ~FileWatcher();

    bool watch(const std::string& directory, bool recursive = true);
    bool unwatch(const std::string& directory);
    void unwatch_all();

    void on_created(FileCreatedCallback cb)   { on_created_ = std::move(cb); }
    void on_modified(FileModifiedCallback cb) { on_modified_ = std::move(cb); }
    void on_deleted(FileDeletedCallback cb)   { on_deleted_ = std::move(cb); }

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    void set_polling_interval_ms(int ms) { poll_interval_ms_ = ms; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> running_{false};
    std::thread worker_;
    int poll_interval_ms_ = 5000;

    FileCreatedCallback  on_created_;
    FileModifiedCallback on_modified_;
    FileDeletedCallback  on_deleted_;

    std::vector<std::string> watch_dirs_;
    bool recursive_ = true;

    std::map<std::string, std::map<std::string, std::filesystem::file_time_type>> file_cache_;
    mutable std::mutex cache_mtx_;
    void poll_filesystem();
};
