#pragma once

#include "core/file_record.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <map>

// Forward declare
class CatalogInterface;

class AccessTracker {
public:
    AccessTracker();
    ~AccessTracker();

    // Start tracking file access events on the given root path.
    // On Windows: ETW (Event Tracing for Windows) - Microsoft-Windows-Kernel-File
    // On Linux: fanotify (requires CAP_SYS_ADMIN) or auditd log parser
    bool start(const std::string& root_path);
    void stop();
    bool is_running() const { return running_.load(); }

    // Set catalog to update on access events
    void set_catalog(CatalogInterface* catalog) { catalog_ = catalog; }

    // Statistics
    int  events_tracked() const { return events_tracked_.load(); }
    void reset_stats() { events_tracked_.store(0); }

    // Fallback: periodic scan of atime changes
    void set_fallback_enabled(bool enabled) { fallback_enabled_ = enabled; }

private:
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::string root_path_;
    CatalogInterface* catalog_ = nullptr;
    std::atomic<int> events_tracked_{0};
    bool fallback_enabled_ = true;

#ifdef _WIN32
    void etw_session();
    // ETW trace session handle
    void* trace_handle_ = nullptr;
#else
    void fanotify_loop();
    int fanotify_fd_ = -1;
#endif

    // Fallback: poll atime via stat()
    void poll_access_times();
    std::map<std::string, time_t> last_atime_cache_;
};
