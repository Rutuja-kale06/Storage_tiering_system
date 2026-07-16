#pragma once

#include "core/file_record.hpp"
#include "migrator_interface.hpp"
#include "policy/policy_engine.hpp"
#include <vector>
#include <chrono>

struct MigrationConstraints {
    double max_bandwidth_mbps     = 100.0;  // throttle bandwidth
    int    max_concurrent_jobs    = 4;      // parallel migrations
    double min_savings_per_job    = 0.001;  // skip trivial migrations ($)
    int    window_start_hour      = 2;      // maintenance window start (2 AM)
    int    window_end_hour        = 5;      // maintenance window end (5 AM)
    bool   window_enabled         = false;  // true = only migrate during window
    double max_duration_minutes   = 60.0;   // max cycle duration
    bool   verify_after_migration = true;   // checksum verify after move
    bool   create_backup          = true;   // backup file before migration
};

struct MigrationJob {
    std::string     file_id;
    std::string     file_path;
    Tier            from_tier;
    Tier            to_tier;
    int64_t         size_bytes;
    double          estimated_duration_sec;
    double          estimated_savings_per_month;
    std::string     reason;
    int             priority;  // higher = migrate first
};

struct MigrationPlan {
    std::vector<MigrationJob> jobs;
    int    total_jobs = 0;
    int64_t total_bytes = 0;
    double  total_estimated_seconds = 0.0;
    double  total_monthly_savings = 0.0;
    double  total_annual_savings = 0.0;
};

class MigrationPlanner {
public:
    MigrationPlanner();

    MigrationConstraints& constraints() { return constraints_; }
    const MigrationConstraints& constraints() const { return constraints_; }

    MigrationPlan plan(const std::vector<PolicyRecommendation>& recommendations);

    // Check if current time is within maintenance window
    bool within_maintenance_window() const;

    // Estimate bandwidth available
    double estimate_bandwidth_mbps() const;

    // Split plan into batches for concurrent execution
    std::vector<std::vector<MigrationJob>> batch_jobs(const MigrationPlan& plan) const;

private:
    MigrationConstraints constraints_;

    double estimate_transfer_time(int64_t bytes, double bandwidth_mbps) const;
    int compute_priority(const PolicyRecommendation& rec) const;
};
