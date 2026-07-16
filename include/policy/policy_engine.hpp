#pragma once

#include "composite_scorer.hpp"
#include "lifecycle_rule.hpp"
#include "catalog/catalog_interface.hpp"
#include <memory>

struct PolicyConfig {
    double hot_threshold     = 15.0;
    double warm_threshold    = 0.0;
    double cold_threshold    = -15.0;
    double archive_threshold = -100.0;
    double age_penalty_days  = 90.0;
    double age_penalty_score = -5.0;
    double critical_bonus    = 10.0;
    double pinned_score      = 30.0;
};

struct PolicyRecommendation {
    FileRecord file;
    Tier       target_tier;
    double     score;
    std::string reason;
    double     estimated_monthly_savings;
};

class PolicyEngine {
public:
    PolicyEngine();

    bool load_config(const std::string& json_path);
    bool load_lifecycle_rules(const std::string& yaml_path);
    bool reload_config();

    // Score a single file
    double score_file(const FileRecord& file) const;

    // Determine target tier for a file
    Tier target_tier(const FileRecord& file) const;

    // Full analysis: score all files and return recommendations
    std::vector<PolicyRecommendation> analyse(const CatalogInterface& catalog, bool dry_run = true) const;

    // Apply lifecycle rules
    std::vector<LifecycleEngine::RuleAction> apply_lifecycle(const std::vector<FileRecord>& files) const;

    // Config access
    PolicyConfig& config() { return config_; }
    const PolicyConfig& config() const { return config_; }
    CompositeScorer& scorer() { return scorer_; }
    const LifecycleEngine& lifecycle() const { return lifecycle_; }

    // Human-readable breakdown
    std::string explain_score(const FileRecord& file) const;

    // Cost projections
    struct CostProjection {
        double current_monthly;
        double optimal_monthly;
        double monthly_savings;
        double annual_savings;
        double savings_percent;
    };
    CostProjection project_costs(const CatalogInterface& catalog) const;

private:
    PolicyConfig config_;
    CompositeScorer scorer_;
    LifecycleEngine lifecycle_;
    std::string config_path_;
    std::string lifecycle_path_;

    ScoringContext make_context() const;
};
