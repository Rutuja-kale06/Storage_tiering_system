#include "tiering/nvram_cache.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>

NvramCache::NvramCache() = default;

int64_t NvramCache::current_size() const {
    int64_t total = 0;
    for (const auto& [_, entry] : buffer_)
        total += entry.data.size();
    return total;
}

bool NvramCache::should_flush() const {
    if (buffer_.empty()) return false;
    if (current_size() >= max_size_bytes_) return true;

    auto oldest = std::min_element(buffer_.begin(), buffer_.end(),
        [](const auto& a, const auto& b) {
            return a.second.first_write < b.second.first_write;
        });

    auto age = std::chrono::steady_clock::now() - oldest->second.first_write;
    return age >= flush_interval_;
}

void NvramCache::evict_oldest() {
    if (buffer_.empty()) return;

    auto oldest = std::min_element(buffer_.begin(), buffer_.end(),
        [](const auto& a, const auto& b) {
            return a.second.first_write < b.second.first_write;
        });

    if (oldest->second.is_dirty && write_back_) {
        write_back_(oldest->second.extent_id,
                    oldest->second.offset,
                    oldest->second.data);
    }
    buffer_.erase(oldest);
}

void NvramCache::write(const std::string& extent_id, int64_t offset,
                        const uint8_t* data, size_t len) {
    if (!enabled_) {
        if (write_back_)
            write_back_(extent_id, offset,
                        std::vector<uint8_t>(data, data + len));
        return;
    }

    auto& entry = buffer_[extent_id];
    if (!entry.is_dirty) {
        entry.extent_id = extent_id;
        entry.offset = offset;
        entry.first_write = std::chrono::steady_clock::now();
        entry.is_dirty = true;
        entry.data.assign(data, data + len);
    } else {
        total_coalesced_.fetch_add(1);
        entry.data.assign(data, data + len);
    }

    if (should_flush())
        flush();
}

bool NvramCache::read(const std::string& extent_id, int64_t offset,
                       uint8_t* buffer, size_t len) const {
    auto it = buffer_.find(extent_id);
    if (it == buffer_.end() || !it->second.is_dirty)
        return false;

    size_t copy_len = std::min(len, it->second.data.size());
    memcpy(buffer, it->second.data.data(), copy_len);
    return true;
}

NvramCache::FlushResult NvramCache::flush() {
    FlushResult result;
    auto start = std::chrono::steady_clock::now();

    for (auto& [extent_id, entry] : buffer_) {
        if (!entry.is_dirty) continue;

        if (write_back_) {
            if (write_back_(extent_id, entry.offset, entry.data)) {
                result.batches_flushed++;
                result.bytes_flushed += entry.data.size();
                result.write_ops_coalesced++;
                entry.is_dirty = false;
            }
        }
    }

    buffer_.clear();
    result.duration_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}

int64_t NvramCache::pending_bytes() const {
    return current_size();
}
