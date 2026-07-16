#pragma once

#include "extent.hpp"
#include <string>
#include <vector>

class HysteresisFilter {
public:
    HysteresisFilter();

    void set_config(const ExtentTierConfig& cfg) { config_ = cfg; }
    const ExtentTierConfig& config() const { return config_; }

    bool should_migrate(const ExtentRecord& extent, Tier target) const;

    struct CostBenefit {
        double benefit;
        double cost;
        double net;
        bool   worth_it;
        std::string reason;
    };
    CostBenefit evaluate(const ExtentRecord& extent, Tier target) const;

    bool is_within_hysteresis(double heat, double threshold) const;

    std::vector<ExtentRecord> filter_candidates(
        const std::vector<ExtentRecord>& candidates,
        Tier target) const;

private:
    ExtentTierConfig config_;

    double compute_benefit(const ExtentRecord& extent, Tier target) const;
    double compute_cost(const ExtentRecord& extent) const;
};
