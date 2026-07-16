#include "tiering/relocation.hpp"
#include "catalog/catalog_interface.hpp"
#include "core/types.hpp"
#include <algorithm>
#include <chrono>
#include <future>

RelocationEngine::RelocationEngine() = default;

int RelocationEngine::compute_priority(const ExtentRecord& extent, Tier target) const {
    double savings = extent.monthly_cost() - extent.size_gb()
                     * tier_cost_per_gb(target) * 30.0;
    return static_cast<int>(savings * 1000.0 + extent.size_mb());
}

RelocationEngine::WatermarkResult RelocationEngine::check_watermarks(
    const std::vector<ExtentRecord>& extents,
    Tier tier,
    const TierWatermark& watermark) const {

    WatermarkResult result;
    int64_t tier_total = 0;
    int64_t tier_used = 0;

    for (const auto& ext : extents) {
        if (ext.current_tier == tier) {
            tier_used += ext.size_bytes;
        }
        if (static_cast<int>(ext.current_tier) >= static_cast<int>(tier)) {
            tier_total += ext.size_bytes;
        }
    }

    if (tier_total > 0) {
        result.current_utilization_percent =
            100.0 * static_cast<double>(tier_used) / tier_total;
    }

    if (result.current_utilization_percent > watermark.max_capacity_percent) {
        result.bytes_to_free = static_cast<int64_t>(
            (result.current_utilization_percent - watermark.max_capacity_percent)
            / 100.0 * tier_total);

        std::vector<ExtentRecord> sorted = extents;
        std::sort(sorted.begin(), sorted.end(),
            [](const ExtentRecord& a, const ExtentRecord& b) {
                return a.heat_score < b.heat_score;
            });

        int64_t freed = 0;
        for (const auto& ext : sorted) {
            if (ext.current_tier != tier) continue;
            if (ext.is_pinned) continue;
            result.must_demote.push_back(ext);
            freed += ext.size_bytes;
            if (freed >= result.bytes_to_free) break;
        }
    }

    return result;
}

RelocationPlan RelocationEngine::plan_relocation(
    const std::vector<ExtentRecord>& promote_candidates,
    const std::vector<ExtentRecord>& demote_candidates) const {

    RelocationPlan plan;

    auto add_jobs = [&](const std::vector<ExtentRecord>& candidates, bool is_promote) {
        for (const auto& ext : candidates) {
            if (!hysteresis_.should_migrate(ext, ext.target_tier))
                continue;

            RelocationJob job;
            job.extent_id = ext.id;
            job.volume_id = ext.volume_id;
            job.size_bytes = ext.size_bytes;
            job.from_tier = ext.current_tier;
            job.to_tier = ext.target_tier;
            job.heat_score = ext.heat_score;
            job.estimated_savings_per_month = ext.monthly_savings();
            job.priority = compute_priority(ext, ext.target_tier);
            if (is_promote)
                job.priority += 10000;
            plan.jobs.push_back(job);
        }
    };

    add_jobs(promote_candidates, true);
    add_jobs(demote_candidates, false);

    std::sort(plan.jobs.begin(), plan.jobs.end(),
        [](const RelocationJob& a, const RelocationJob& b) {
            return a.priority > b.priority;
        });

    plan.total_jobs = static_cast<int>(plan.jobs.size());
    for (const auto& j : plan.jobs) {
        plan.total_bytes += j.size_bytes;
        plan.total_monthly_savings += j.estimated_savings_per_month;
    }

    return plan;
}

bool RelocationEngine::copy_extent_data(const ExtentRecord& extent,
                                         const std::string& dest_path) {
    return true;
}

bool RelocationEngine::verify_extent(const ExtentRecord& extent) {
    return true;
}

RelocationEngine::RelocateResult RelocationEngine::relocate_extent(
    ExtentRecord& extent, Tier target_tier,
    std::function<bool(const std::string&)> verify_cb) {

    RelocateResult result;
    auto start = std::chrono::steady_clock::now();

    extent.target_tier = target_tier;

    if (catalog_) {
        catalog_->update_extent(extent);

        catalog_->log_extent_migration(extent.id, extent.volume_id,
            extent.current_tier, target_tier, extent.size_bytes,
            "heat_map_relocation");
    }

    extent.current_tier = target_tier;
    extent.last_migrated = std::time(nullptr);

    if (wear_leveler_) {
        wear_leveler_->record_pe_cycle(extent.id);
    }

    result.duration_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    result.success = true;

    return result;
}

RelocationEngine::BatchResult RelocationEngine::execute_batch(
    const std::vector<RelocationJob>& batch) {

    BatchResult result;
    auto start = std::chrono::steady_clock::now();

    std::vector<std::pair<std::future<RelocateResult>, int64_t>> futures;
    for (const auto& job : batch) {
        futures.push_back({std::async(std::launch::async,
            [this, &job]() {
                ExtentRecord ext;
                ext.id = job.extent_id;
                ext.volume_id = job.volume_id;
                ext.size_bytes = job.size_bytes;
                ext.current_tier = job.from_tier;
                ext.heat_score = job.heat_score;
                return relocate_extent(ext, job.to_tier);
            }), job.size_bytes});
    }

    for (auto& [f, size] : futures) {
        auto r = f.get();
        if (r.success) {
            result.succeeded++;
            result.bytes_moved += size;
        } else {
            result.failed++;
        }
    }

    result.total_duration_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}

void RelocationEngine::simulate_relocation(ExtentRecord& extent, Tier target_tier) {
    extent.prev_heat_score = extent.heat_score;
    extent.current_tier = target_tier;
    extent.target_tier = target_tier;
    extent.last_migrated = std::time(nullptr);

    if (catalog_)
        catalog_->update_extent(extent);
}
