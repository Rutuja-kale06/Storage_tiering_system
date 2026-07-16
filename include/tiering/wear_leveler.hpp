#pragma once

#include "extent.hpp"
#include <string>
#include <vector>
#include <unordered_map>

class WearLeveler {
public:
    WearLeveler();

    void set_pe_limit(int64_t limit) { pe_limit_ = limit; }
    void set_balance_threshold(double threshold) { balance_threshold_ = threshold; }

    void record_pe_cycle(const std::string& extent_id);
    int64_t pe_cycles(const std::string& extent_id) const;

    std::string find_best_flash_target(
        const std::vector<ExtentRecord>& flash_extents) const;

    bool is_balanced(const std::vector<ExtentRecord>& flash_extents) const;

    struct WearReport {
        int64_t min_pe;
        int64_t max_pe;
        int64_t avg_pe;
        double  imbalance_ratio;
        bool    needs_rebalance;
        int     extents_nearing_end_of_life;
    };
    WearReport report(const std::vector<ExtentRecord>& flash_extents) const;

    int64_t remaining_life(const std::string& extent_id) const;

private:
    int64_t pe_limit_ = 100000;
    double balance_threshold_ = 0.2;
    std::unordered_map<std::string, int64_t> pe_counts_;
};
