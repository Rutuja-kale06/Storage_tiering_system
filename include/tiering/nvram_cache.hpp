#pragma once

#include "extent.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <functional>

class NvramCache {
public:
    NvramCache();

    void set_max_size_mb(int mb) {
        max_size_bytes_ = static_cast<int64_t>(mb) * 1024 * 1024;
    }
    void set_flush_interval_ms(int ms) {
        flush_interval_ = std::chrono::milliseconds(ms);
    }
    void set_enabled(bool enabled) { enabled_ = enabled; }

    void write(const std::string& extent_id, int64_t offset,
               const uint8_t* data, size_t len);

    bool read(const std::string& extent_id, int64_t offset,
              uint8_t* buffer, size_t len) const;

    struct FlushResult {
        int     batches_flushed = 0;
        int64_t bytes_flushed = 0;
        int     write_ops_coalesced = 0;
        double  duration_ms = 0.0;
    };
    FlushResult flush();

    int  pending_writes() const { return static_cast<int>(buffer_.size()); }
    int64_t pending_bytes() const;
    int  total_coalesced() const { return total_coalesced_.load(); }

    using WriteBack = std::function<bool(const std::string& extent_id,
                                          int64_t offset,
                                          const std::vector<uint8_t>& data)>;
    void set_write_back(WriteBack cb) { write_back_ = std::move(cb); }

private:
    struct CacheEntry {
        std::string extent_id;
        int64_t offset;
        std::vector<uint8_t> data;
        std::chrono::steady_clock::time_point first_write;
        bool is_dirty = false;
    };

    bool enabled_ = true;
    int64_t max_size_bytes_ = 64LL * 1024 * 1024;
    std::chrono::milliseconds flush_interval_{100};
    std::unordered_map<std::string, CacheEntry> buffer_;
    std::atomic<int> total_coalesced_{0};
    WriteBack write_back_;

    int64_t current_size() const;
    bool should_flush() const;
    void evict_oldest();
};
