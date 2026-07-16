#include "policy/policy_engine.hpp"
#include "core/types.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

PolicyEngine::PolicyEngine() = default;

ScoringContext PolicyEngine::make_context() const {
    ScoringContext ctx;
    ctx.hot_threshold     = config_.hot_threshold;
    ctx.warm_threshold    = config_.warm_threshold;
    ctx.cold_threshold    = config_.cold_threshold;
    ctx.archive_threshold = config_.archive_threshold;
    return ctx;
}

bool PolicyEngine::load_config(const std::string& json_path) {
    config_path_ = json_path;
    return reload_config();
}

bool PolicyEngine::load_lifecycle_rules(const std::string& yaml_path) {
    lifecycle_path_ = yaml_path;
    return lifecycle_.load_rules(yaml_path);
}

bool PolicyEngine::reload_config() {
    if (config_path_.empty()) return false;

    try {
        std::ifstream f(config_path_);
        if (!f.is_open()) return false;

        nlohmann::json cfg;
        f >> cfg;

        // Scorers
        if (cfg.contains("scorers") && cfg["scorers"].is_array()) {
            scorer_.load_config(cfg["scorers"]);
        }

        // Tier thresholds
        if (cfg.contains("tier_thresholds")) {
            auto& t = cfg["tier_thresholds"];
            if (t.contains("HOT"))    config_.hot_threshold     = t["HOT"]["min_score"].get<double>();
            if (t.contains("WARM"))   config_.warm_threshold    = t["WARM"]["min_score"].get<double>();
            if (t.contains("COLD"))   config_.cold_threshold    = t["COLD"]["min_score"].get<double>();
            if (t.contains("ARCHIVE"))config_.archive_threshold = t["ARCHIVE"]["min_score"].get<double>();
        }

        // Global settings
        if (cfg.contains("age_penalty_days"))  config_.age_penalty_days  = cfg["age_penalty_days"].get<double>();
        if (cfg.contains("age_penalty_score")) config_.age_penalty_score = cfg["age_penalty_score"].get<double>();
        if (cfg.contains("critical_bonus"))    config_.critical_bonus    = cfg["critical_bonus"].get<double>();
        if (cfg.contains("pinned_score"))      config_.pinned_score      = cfg["pinned_score"].get<double>();

        // Lifecycle
        if (cfg.contains("lifecycle_rules_path")) {
            load_lifecycle_rules(cfg["lifecycle_rules_path"].get<std::string>());
        }

        return true;
    } catch (...) {
        return false;
    }
}

double PolicyEngine::score_file(const FileRecord& file) const {
    if (file.is_pinned) return config_.pinned_score;

    auto ctx = make_context();
    double s = scorer_.score(file, ctx);

    // Age penalty (older files score lower, promoting demotion)
    if (file.age_days() > config_.age_penalty_days)
        s -= config_.age_penalty_score;

    // Critical flag bonus
    if (file.is_critical)
        s += config_.critical_bonus;

    return s;
}

Tier PolicyEngine::target_tier(const FileRecord& file) const {
    double s = score_file(file);
    if (s >= config_.hot_threshold)      return Tier::HOT;
    if (s >= config_.warm_threshold)     return Tier::WARM;
    if (s >= config_.cold_threshold)     return Tier::COLD;
    return Tier::ARCHIVE;
}

std::vector<PolicyRecommendation> PolicyEngine::analyse(
    const CatalogInterface& catalog, bool dry_run) const
{
    std::vector<PolicyRecommendation> recs;
    auto files = catalog.all_files();

    for (auto& file : files) {
        if (file.is_pinned) continue;

        Tier target = target_tier(file);
        if (target == file.current_tier) continue;

        double s = score_file(file);
        PolicyRecommendation rec;
        rec.file          = file;
        rec.target_tier   = target;
        rec.score         = s;
        rec.reason        = explain_score(file);

        // Monthly savings estimate
        double current_cost = file.size_gb() * tier_cost_per_gb(file.current_tier) * 30.0;
        double target_cost  = file.size_gb() * tier_cost_per_gb(target) * 30.0;
        rec.estimated_monthly_savings = current_cost - target_cost;

        recs.push_back(rec);
    }

    // Sort by savings descending (largest impact first)
    std::sort(recs.begin(), recs.end(),
        [](const PolicyRecommendation& a, const PolicyRecommendation& b) {
            return a.estimated_monthly_savings > b.estimated_monthly_savings;
        });

    return recs;
}

std::vector<LifecycleEngine::RuleAction> PolicyEngine::apply_lifecycle(
    const std::vector<FileRecord>& files) const
{
    return lifecycle_.evaluate(files);
}

std::string PolicyEngine::explain_score(const FileRecord& file) const {
    if (file.is_pinned) return "pinned=+30.0 (forced HOT)";

    auto ctx = make_context();
    std::string s = scorer_.explain(file);

    // Add age penalty note
    if (file.age_days() > config_.age_penalty_days) {
        s += " age_penalty=" + std::to_string(static_cast<int>(config_.age_penalty_score));
    }
    if (file.is_critical) {
        s += " critical=+" + std::to_string(static_cast<int>(config_.critical_bonus));
    }

    return s;
}

PolicyEngine::CostProjection PolicyEngine::project_costs(const CatalogInterface& catalog) const {
    CostProjection proj;
    proj.current_monthly = catalog.total_monthly_cost();

    // Compute optimal cost: for each file, compute target tier cost
    double optimal = 0.0;
    auto files = catalog.all_files();
    for (auto& f : files) {
        Tier target = f.is_pinned ? Tier::HOT : target_tier(f);
        optimal += f.size_gb() * tier_cost_per_gb(target) * 30.0;
    }

    proj.optimal_monthly = optimal;
    proj.monthly_savings = proj.current_monthly - proj.optimal_monthly;
    proj.annual_savings  = proj.monthly_savings * 12.0;

    if (proj.current_monthly > 0)
        proj.savings_percent = (proj.monthly_savings / proj.current_monthly) * 100.0;
    else
        proj.savings_percent = 0.0;

    return proj;
}
