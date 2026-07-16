#pragma once

#include "extent.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <deque>
#include <functional>

class CatalogInterface;

class HeatMapEngine {
public:
    HeatMapEngine();

    void set_config(const ExtentTierConfig& cfg) { config_ = cfg; }
    const ExtentTierConfig& config() const { return config_; }

    void record_iops(const std::string& extent_id, int64_t iops_read, int64_t iops_write);
    void record_lba(const std::string& extent_id, int64_t lba);

    struct AnalysisResult {
        std::vector<ExtentRecord> promote_candidates;
        std::vector<ExtentRecord> demote_candidates;
        int    total_analysed = 0;
        double avg_heat = 0.0;
        double max_heat = 0.0;
    };

    AnalysisResult analyse(const std::vector<ExtentRecord>& extents);

    Tier target_tier(const ExtentRecord& extent) const;

    void flush_checkpoint(CatalogInterface& catalog);

    double get_heat(const std::string& extent_id) const;
    double get_latest_iops(const std::string& extent_id) const;

    void reset();

    void inject_synthetic_iops(const std::string& extent_id, int64_t iops, int64_t lba);

private:
    struct ExtentHeatState {
        double heat_score = 0.0;
        double latest_iops = 0.0;
        std::deque<IOSample> recent_samples;
        int64_t last_lba = 0;
        int64_t sequential_count = 0;
        int64_t total_io_count = 0;
    };

    ExtentTierConfig config_;
    std::unordered_map<std::string, ExtentHeatState> states_;

    double compute_ema(double current_iops, double prev_heat) const;
};
