#include "tiering/io_profiler.hpp"
#include <algorithm>
#include <cmath>

IoProfiler::IoProfiler() = default;

bool IoProfiler::is_sequential(int64_t prev_lba, int64_t prev_size,
                                int64_t curr_lba) const {
    int64_t expected_next = prev_lba + prev_size;
    int64_t gap = std::abs(curr_lba - expected_next);
    return gap <= sequential_gap_;
}

void IoProfiler::record_access(const std::string& extent_id,
                                int64_t lba, int64_t size) {
    auto& pattern = patterns_[extent_id];
    pattern.total_count++;

    if (!pattern.recent_accesses.empty()) {
        auto [prev_lba, prev_size] = pattern.recent_accesses.back();
        if (is_sequential(prev_lba, prev_size, lba))
            pattern.sequential_count++;
    }

    pattern.recent_accesses.push_back({lba, size});
    if (static_cast<int>(pattern.recent_accesses.size()) > max_samples_)
        pattern.recent_accesses.pop_front();
}

double IoProfiler::sequential_ratio(const std::string& extent_id) const {
    auto it = patterns_.find(extent_id);
    if (it == patterns_.end() || it->second.total_count < 2)
        return 0.5;
    return static_cast<double>(it->second.sequential_count)
           / static_cast<double>(it->second.total_count - 1);
}

IoProfiler::IOType IoProfiler::classify(const std::string& extent_id) const {
    double ratio = sequential_ratio(extent_id);
    if (ratio >= sequential_threshold_) return IOType::SEQUENTIAL;
    if (ratio >= (1.0 - sequential_threshold_)) return IOType::MIXED;
    return IOType::RANDOM;
}

void IoProfiler::reset(const std::string& extent_id) {
    patterns_.erase(extent_id);
}
