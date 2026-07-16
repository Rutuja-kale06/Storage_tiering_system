#include "migration/migration_planner.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>

MigrationPlanner::MigrationPlanner() = default;

double MigrationPlanner::estimate_transfer_time(int64_t bytes, double bandwidth_mbps) const {
    if (bandwidth_mbps <= 0) return 0.0;
    double bits = static_cast<double>(bytes) * 8.0;
    double bps = bandwidth_mbps * 1'000'000.0;
    return bits / bps;
}

int MigrationPlanner::compute_priority(const PolicyRecommendation& rec) const {
    int p = 0;
    // Higher savings → higher priority
    p += static_cast<int>(rec.estimated_monthly_savings * 1000);
    // Larger files first (maximize impact per operation)
    p += static_cast<int>(rec.file.size_bytes / (1024 * 1024));
    return p;
}

bool MigrationPlanner::within_maintenance_window() const {
    if (!constraints_.window_enabled) return true;

    time_t now = std::time(nullptr);
    struct tm* tm_info = std::localtime(&now);
    int hour = tm_info->tm_hour;

    if (constraints_.window_start_hour <= constraints_.window_end_hour) {
        return hour >= constraints_.window_start_hour &&
               hour < constraints_.window_end_hour;
    }
    // Wraparound (e.g., 22:00 - 06:00)
    return hour >= constraints_.window_start_hour ||
           hour < constraints_.window_end_hour;
}

double MigrationPlanner::estimate_bandwidth_mbps() const {
    // In production, this would probe actual I/O bandwidth
    // For now, return configured value
    return constraints_.max_bandwidth_mbps;
}

MigrationPlan MigrationPlanner::plan(const std::vector<PolicyRecommendation>& recommendations) {
    MigrationPlan plan;

    for (const auto& rec : recommendations) {
        // Skip migrations below savings threshold
        if (rec.estimated_monthly_savings < constraints_.min_savings_per_job)
            continue;

        MigrationJob job;
        job.file_id                   = rec.file.id;
        job.file_path                 = rec.file.path;
        job.from_tier                 = rec.file.current_tier;
        job.to_tier                   = rec.target_tier;
        job.size_bytes                = rec.file.size_bytes;
        job.estimated_savings_per_month = rec.estimated_monthly_savings;
        job.reason                    = rec.reason;
        job.priority                  = compute_priority(rec);
        job.estimated_duration_sec    = estimate_transfer_time(
            rec.file.size_bytes, estimate_bandwidth_mbps());

        plan.jobs.push_back(job);
        plan.total_jobs++;
        plan.total_bytes += job.size_bytes;
        plan.total_estimated_seconds += job.estimated_duration_sec;
        plan.total_monthly_savings += rec.estimated_monthly_savings;
        plan.total_annual_savings += rec.estimated_monthly_savings * 12.0;
    }

    // Sort by priority descending
    std::sort(plan.jobs.begin(), plan.jobs.end(),
        [](const MigrationJob& a, const MigrationJob& b) {
            return a.priority > b.priority;
        });

    return plan;
}

std::vector<std::vector<MigrationJob>> MigrationPlanner::batch_jobs(
    const MigrationPlan& plan) const
{
    std::vector<std::vector<MigrationJob>> batches;
    if (plan.jobs.empty()) return batches;

    int num_batches = (plan.jobs.size() + constraints_.max_concurrent_jobs - 1)
                       / constraints_.max_concurrent_jobs;
    if (num_batches < 1) num_batches = 1;

    int jobs_per_batch = (plan.jobs.size() + num_batches - 1) / num_batches;
    if (jobs_per_batch < 1) jobs_per_batch = 1;

    for (size_t i = 0; i < plan.jobs.size(); i += jobs_per_batch) {
        std::vector<MigrationJob> batch;
        for (size_t j = i; j < plan.jobs.size() && j < i + jobs_per_batch; ++j)
            batch.push_back(plan.jobs[j]);
        batches.push_back(batch);
    }

    return batches;
}
