#pragma once

#include "scorer.hpp"
#include <memory>
#include <vector>

class CompositeScorer : public Scorer {
public:
    CompositeScorer();

    std::string name() const override { return "composite"; }

    void add_scorer(std::unique_ptr<Scorer> scorer);
    void clear_scorers();

    double score(const FileRecord& file, const ScoringContext& ctx) const override;
    std::string explain(const FileRecord& file) const override;

    // Load scorer configuration from JSON array
    bool load_config(const nlohmann::json& config) override;

    const std::vector<std::unique_ptr<Scorer>>& scorers() const { return scorers_; }

private:
    std::vector<std::unique_ptr<Scorer>> scorers_;
};
