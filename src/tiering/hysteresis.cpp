#include "tiering/hysteresis.hpp"
#include <cmath>
#include <algorithm>

HysteresisFilter::HysteresisFilter() = default;

bool HysteresisFilter::is_within_hysteresis(double heat, double threshold) const {
    return std::abs(heat - threshold) <= config_.hysteresis_band;
}

double HysteresisFilter::compute_benefit(const ExtentRecord& extent, Tier target) const {
    double current_latency = tier_latency_ms(extent.current_tier);
    double target_latency = tier_latency_ms(target);
    double latency_improvement = std::max(0.0, current_latency - target_latency);

    double iops = extent.heat_score;
    if (iops <= 0) iops = 1.0;

    return latency_improvement * iops * config_.expected_spike_seconds;
}

double HysteresisFilter::compute_cost(const ExtentRecord& extent) const {
    double size_gb = extent.size_gb();
    double wear = config_.wear_penalty_per_gb * size_gb;
    double transfer_seconds = (extent.size_bytes / 1.0)
                              / (config_.bandwidth_mbps * 1024.0 * 1024.0 / 8.0);

    double migration_overhead = transfer_seconds * 0.001;
    return wear + migration_overhead;
}

HysteresisFilter::CostBenefit HysteresisFilter::evaluate(
    const ExtentRecord& extent, Tier target) const {
    CostBenefit result;
    result.benefit = compute_benefit(extent, target);
    result.cost = compute_cost(extent);
    result.net = result.benefit - result.cost;

    if (result.net > 0) {
        result.worth_it = true;
        result.reason = "benefit exceeds cost";
    } else {
        result.worth_it = false;
        result.reason = "cost exceeds benefit";
    }

    return result;
}

bool HysteresisFilter::should_migrate(const ExtentRecord& extent, Tier target) const {
    double threshold = 0.0;
    switch (target) {
        case Tier::HOT:    threshold = 15.0; break;
        case Tier::WARM:   threshold = 0.0;  break;
        case Tier::COLD:   threshold = -15.0; break;
        case Tier::ARCHIVE: threshold = -100.0; break;
    }

    if (static_cast<int>(target) < static_cast<int>(extent.current_tier)) {
        if (extent.heat_score <= threshold + config_.hysteresis_band)
            return false;
    } else {
        if (extent.heat_score >= threshold - config_.hysteresis_band)
            return false;
    }

    auto cb = evaluate(extent, target);
    return cb.worth_it;
}

std::vector<ExtentRecord> HysteresisFilter::filter_candidates(
    const std::vector<ExtentRecord>& candidates, Tier target) const {
    std::vector<ExtentRecord> filtered;
    for (const auto& ext : candidates) {
        if (should_migrate(ext, target))
            filtered.push_back(ext);
    }
    return filtered;
}
