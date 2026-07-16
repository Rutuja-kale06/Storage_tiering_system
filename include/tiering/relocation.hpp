#pragma once

#include "extent.hpp"
#include "hysteresis.hpp"
#include "heat_map.hpp"
#include "wear_leveler.hpp"
#include <string>
#include <vector>
#include <memory>
#include <functional>

class CatalogInterface;

class RelocationEngine {
public:
    RelocationEngine();

    void set_catalog(CatalogInterface* catalog) { catalog_ = catalog; }
    void set_heat_map(std::shared_ptr<HeatMapEngine> heat_map) { heat_map_ = heat_map; }
    void set_wear_leveler(std::shared_ptr<WearLeveler> wear_leveler) { wear_leveler_ = wear_leveler; }
    void set_config(const ExtentTierConfig& cfg) { config_ = cfg; hysteresis_.set_config(cfg); }

    struct WatermarkResult {
        std::vector<ExtentRecord> must_demote;
        int64_t    bytes_to_free;
        double     current_utilization_percent;
    };
    WatermarkResult check_watermarks(const std::vector<ExtentRecord>& extents,
                                     Tier tier,
                                     const TierWatermark& watermark) const;

    RelocationPlan plan_relocation(
        const std::vector<ExtentRecord>& promote_candidates,
        const std::vector<ExtentRecord>& demote_candidates) const;

    struct RelocateResult {
        bool   success = false;
        std::string error;
        double duration_ms = 0.0;
    };
    RelocateResult relocate_extent(ExtentRecord& extent, Tier target_tier,
                                    std::function<bool(const std::string&)> verify_cb = nullptr);

    struct BatchResult {
        int     succeeded = 0;
        int     failed = 0;
        int64_t bytes_moved = 0;
        double  total_duration_ms = 0.0;
    };
    BatchResult execute_batch(const std::vector<RelocationJob>& batch);

    void simulate_relocation(ExtentRecord& extent, Tier target_tier);

private:
    CatalogInterface* catalog_ = nullptr;
    std::shared_ptr<HeatMapEngine> heat_map_;
    std::shared_ptr<WearLeveler> wear_leveler_;
    HysteresisFilter hysteresis_;
    ExtentTierConfig config_;

    int compute_priority(const ExtentRecord& extent, Tier target) const;
    bool copy_extent_data(const ExtentRecord& extent, const std::string& dest_path);
    bool verify_extent(const ExtentRecord& extent);
};
