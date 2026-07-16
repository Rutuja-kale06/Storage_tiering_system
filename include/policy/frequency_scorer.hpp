#pragma once

#include "scorer.hpp"

class FrequencyScorer : public Scorer {
public:
    FrequencyScorer();

    std::string name() const override { return "frequency"; }
    double score(const FileRecord& file, const ScoringContext& ctx) const override;
    std::string explain(const FileRecord& file) const override;
    bool load_config(const nlohmann::json& config) override;

private:
    double multiplier_ = 3.0;
    double max_bonus_  = 12.0;
};
