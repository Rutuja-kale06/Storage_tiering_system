#pragma once

#include "core/tier.hpp"
#include <string>
#include <cstdint>
#include <ctime>
#include <vector>
#include <deque>
#include <cmath>
#include <unordered_map>

struct IOSample {
    time_t  timestamp;
    int64_t iops_read;
    int64_t iops_write;
    int64_t bytes_read;
    int64_t bytes_written;
    int64_t lba;
    double  latency_us;
};

struct ExtentRecord {
    std::string id;
    std::string volume_id;
    int64_t     offset_bytes;
    int64_t     size_bytes;

    Tier        current_tier;
    Tier        target_tier;

    int64_t     iops_read;
    int64_t     iops_write;
    int64_t     bytes_read;
    int64_t     bytes_written;

    double      heat_score;
    double      prev_heat_score;

    double      sequential_ratio;
    int64_t     last_lba;

    int64_t     pe_cycles;

    bool        is_pinned;
    time_t      last_migrated;
    time_t      last_sampled;
    int         sample_count;

    std::string physical_path;
    int64_t     physical_offset;

    double size_mb()  const { return size_bytes / (1024.0 * 1024.0); }
    double size_gb()  const { return size_bytes / (1024.0 * 1024.0 * 1024.0); }

    double monthly_cost() const {
        return size_gb() * tier_cost_per_gb(current_tier) * 30.0;
    }

    double target_cost() const {
        return size_gb() * tier_cost_per_gb(target_tier) * 30.0;
    }

    double monthly_savings() const {
        return monthly_cost() - target_cost();
    }
};

struct VolumeRecord {
    std::string id;
    std::string path;
    int64_t     total_size;
    int64_t     extent_size;
    std::string label;
    bool        is_active;
    time_t      registered_at;

    int extent_count() const {
        return static_cast<int>(std::ceil(static_cast<double>(total_size) / extent_size));
    }
};

struct ExtentTierConfig {
    int64_t extent_size_bytes = 256LL * 1024 * 1024;
    double  ema_alpha = 0.3;
    int     sampling_interval_seconds = 5;
    int     analysis_interval_minutes = 60;
    int     relocation_window_start_hour = 2;
    int     relocation_window_end_hour = 5;
    double  hysteresis_band = 5.0;
    double  expected_spike_seconds = 300.0;
    double  sequential_threshold = 0.7;
    int64_t wear_limit_pe_cycles = 100000;
    int     max_samples_per_extent = 1000;
    double  bandwidth_mbps = 100.0;
    double  wear_penalty_per_gb = 0.5;
};

struct TierWatermark {
    double  max_capacity_percent = 85.0;
    int64_t iops_capacity = 40000;
};

struct RelocationJob {
    std::string extent_id;
    std::string volume_id;
    int64_t     size_bytes;
    Tier        from_tier;
    Tier        to_tier;
    double      heat_score;
    double      estimated_savings_per_month;
    int         priority;
};

struct RelocationPlan {
    std::vector<RelocationJob> jobs;
    int     total_jobs = 0;
    int64_t total_bytes = 0;
    double  total_monthly_savings = 0.0;
};
