#pragma once

#include "scorer.hpp"

class SizeScorer : public Scorer {
public:
    SizeScorer();

    std::string name() const override { return "size"; }
    double score(const FileRecord& file, const ScoringContext& ctx) const override;
    std::string explain(const FileRecord& file) const override;
    bool load_config(const nlohmann::json& config) override;

private:
    double large_size_gb_ = 5.0;
    double huge_size_gb_  = 20.0;
    double large_penalty_ = -5.0;
    double huge_penalty_  = -10.0;
};
