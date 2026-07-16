#pragma once

#include "extent.hpp"
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

class IoProfiler {
public:
    IoProfiler();

    void set_sequential_threshold(double ratio) { sequential_threshold_ = ratio; }
    void set_max_samples(int n) { max_samples_ = n; }
    void set_sequential_gap(int64_t bytes) { sequential_gap_ = bytes; }

    void record_access(const std::string& extent_id, int64_t lba, int64_t size);

    double sequential_ratio(const std::string& extent_id) const;

    enum class IOType {
        RANDOM,
        MIXED,
        SEQUENTIAL
    };
    IOType classify(const std::string& extent_id) const;

    void reset(const std::string& extent_id);

private:
    struct AccessPattern {
        std::deque<std::pair<int64_t, int64_t>> recent_accesses;
        int64_t sequential_count = 0;
        int64_t total_count = 0;
    };

    double sequential_threshold_ = 0.7;
    int max_samples_ = 100;
    int64_t sequential_gap_ = 4096;

    std::unordered_map<std::string, AccessPattern> patterns_;

    bool is_sequential(int64_t prev_lba, int64_t prev_size, int64_t curr_lba) const;
};
