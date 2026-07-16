#include "tiering/heat_map.hpp"
#include "catalog/catalog_interface.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

HeatMapEngine::HeatMapEngine() = default;

double HeatMapEngine::compute_ema(double current_iops, double prev_heat) const {
    return config_.ema_alpha * current_iops + (1.0 - config_.ema_alpha) * prev_heat;
}

void HeatMapEngine::record_iops(const std::string& extent_id,
                                 int64_t iops_read, int64_t iops_write) {
    auto& state = states_[extent_id];
    double current_iops = static_cast<double>(iops_read + iops_write);
    state.latest_iops = current_iops;

    state.recent_samples.push_back({std::time(nullptr), iops_read, iops_write,
                                    0, 0, state.last_lba, 0.0});
    if (static_cast<int>(state.recent_samples.size()) > config_.max_samples_per_extent)
        state.recent_samples.pop_front();

    state.heat_score = compute_ema(current_iops, state.heat_score);
}

void HeatMapEngine::record_lba(const std::string& extent_id, int64_t lba) {
    auto& state = states_[extent_id];
    state.total_io_count++;

    if (state.total_io_count > 1) {
        int64_t gap = lba - state.last_lba;
        if (gap >= 0 && gap <= 4096)
            state.sequential_count++;
    }
    state.last_lba = lba;
}

Tier HeatMapEngine::target_tier(const ExtentRecord& extent) const {
    auto it = states_.find(extent.id);
    double heat = (it != states_.end()) ? it->second.heat_score : extent.heat_score;
    double seq_ratio = extent.sequential_ratio;

    bool is_sequential = seq_ratio >= config_.sequential_threshold;

    if (extent.is_pinned)
        return Tier::HOT;

    if (heat >= 15.0) {
        if (is_sequential)
            return Tier::WARM;
        return Tier::HOT;
    }
    if (heat >= 0.0)  return Tier::WARM;
    if (heat >= -15.0) return Tier::COLD;
    return Tier::ARCHIVE;
}

HeatMapEngine::AnalysisResult HeatMapEngine::analyse(
    const std::vector<ExtentRecord>& extents) {
    AnalysisResult result;
    double heat_sum = 0.0;
    double heat_max = -std::numeric_limits<double>::max();

    for (const auto& extent : extents) {
        auto it = states_.find(extent.id);
        double heat = (it != states_.end()) ? it->second.heat_score : extent.heat_score;
        heat_sum += heat;
        heat_max = std::max(heat_max, heat);
        result.total_analysed++;

        Tier target = target_tier(extent);
        if (target != extent.current_tier) {
            ExtentRecord copy = extent;
            copy.heat_score = heat;
            copy.target_tier = target;
            if (static_cast<int>(target) < static_cast<int>(extent.current_tier))
                result.promote_candidates.push_back(copy);
            else
                result.demote_candidates.push_back(copy);
        }
    }

    result.avg_heat = result.total_analysed > 0 ? heat_sum / result.total_analysed : 0.0;
    result.max_heat = heat_max;
    return result;
}

void HeatMapEngine::flush_checkpoint(CatalogInterface& catalog) {
    for (const auto& [extent_id, state] : states_) {
        for (const auto& sample : state.recent_samples) {
            catalog.log_io_sample(extent_id, sample.timestamp,
                                  sample.iops_read, sample.iops_write,
                                  sample.bytes_read, sample.bytes_written,
                                  sample.latency_us);
        }
    }
}

double HeatMapEngine::get_heat(const std::string& extent_id) const {
    auto it = states_.find(extent_id);
    return (it != states_.end()) ? it->second.heat_score : 0.0;
}

double HeatMapEngine::get_latest_iops(const std::string& extent_id) const {
    auto it = states_.find(extent_id);
    return (it != states_.end()) ? it->second.latest_iops : 0.0;
}

void HeatMapEngine::reset() {
    states_.clear();
}

void HeatMapEngine::inject_synthetic_iops(const std::string& extent_id,
                                           int64_t iops, int64_t lba) {
    record_iops(extent_id, iops, iops / 2);
    if (lba >= 0) record_lba(extent_id, lba);
}
