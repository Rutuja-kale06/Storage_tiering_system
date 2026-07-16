#pragma once

#include "scorer.hpp"
#include <vector>

class RecencyScorer : public Scorer {
public:
    struct Threshold {
        double max_idle_days;
        double weight;
    };

    RecencyScorer();

    std::string name() const override { return "recency"; }
    double score(const FileRecord& file, const ScoringContext& ctx) const override;
    std::string explain(const FileRecord& file) const override;
    bool load_config(const nlohmann::json& config) override;

    const std::vector<Threshold>& thresholds() const { return thresholds_; }
    void log_warning_if_default() const;

private:
    std::vector<Threshold> thresholds_;
};
