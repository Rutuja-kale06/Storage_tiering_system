#include "tiering/wear_leveler.hpp"
#include <algorithm>
#include <limits>

WearLeveler::WearLeveler() = default;

void WearLeveler::record_pe_cycle(const std::string& extent_id) {
    pe_counts_[extent_id]++;
}

int64_t WearLeveler::pe_cycles(const std::string& extent_id) const {
    auto it = pe_counts_.find(extent_id);
    return (it != pe_counts_.end()) ? it->second : 0;
}

std::string WearLeveler::find_best_flash_target(
    const std::vector<ExtentRecord>& flash_extents) const {
    if (flash_extents.empty()) return {};

    auto it = std::min_element(flash_extents.begin(), flash_extents.end(),
        [this](const ExtentRecord& a, const ExtentRecord& b) {
            return pe_cycles(a.id) < pe_cycles(b.id);
        });
    return it->id;
}

bool WearLeveler::is_balanced(
    const std::vector<ExtentRecord>& flash_extents) const {
    if (flash_extents.size() < 2) return true;

    int64_t min_pe = std::numeric_limits<int64_t>::max();
    int64_t max_pe = 0;

    for (const auto& ext : flash_extents) {
        int64_t pe = pe_cycles(ext.id);
        min_pe = std::min(min_pe, pe);
        max_pe = std::max(max_pe, pe);
    }

    if (min_pe == 0) min_pe = 1;
    double ratio = static_cast<double>(max_pe) / min_pe;
    return ratio <= (1.0 + balance_threshold_);
}

WearLeveler::WearReport WearLeveler::report(
    const std::vector<ExtentRecord>& flash_extents) const {
    WearReport r;
    if (flash_extents.empty()) return r;

    r.min_pe = std::numeric_limits<int64_t>::max();
    r.max_pe = 0;
    r.avg_pe = 0;
    r.extents_nearing_end_of_life = 0;

    for (const auto& ext : flash_extents) {
        int64_t pe = pe_cycles(ext.id);
        r.min_pe = std::min(r.min_pe, pe);
        r.max_pe = std::max(r.max_pe, pe);
        r.avg_pe += pe;
        if (pe >= pe_limit_ * 0.9)
            r.extents_nearing_end_of_life++;
    }

    r.avg_pe /= flash_extents.size();
    int64_t min = (r.min_pe == 0) ? 1 : r.min_pe;
    r.imbalance_ratio = static_cast<double>(r.max_pe) / min;
    r.needs_rebalance = r.imbalance_ratio > (1.0 + balance_threshold_);

    return r;
}

int64_t WearLeveler::remaining_life(const std::string& extent_id) const {
    int64_t current = pe_cycles(extent_id);
    return std::max(0LL, pe_limit_ - current);
}
