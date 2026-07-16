#include "policy/composite_scorer.hpp"
#include "policy/recency_scorer.hpp"
#include "policy/frequency_scorer.hpp"
#include "policy/type_scorer.hpp"
#include "policy/size_scorer.hpp"
#include <sstream>
#include <iomanip>

CompositeScorer::CompositeScorer() {
    // Default scorer chain
    add_scorer(std::make_unique<RecencyScorer>());
    add_scorer(std::make_unique<FrequencyScorer>());
    add_scorer(std::make_unique<TypeScorer>());
    add_scorer(std::make_unique<SizeScorer>());
}

void CompositeScorer::add_scorer(std::unique_ptr<Scorer> scorer) {
    scorers_.push_back(std::move(scorer));
}

void CompositeScorer::clear_scorers() {
    scorers_.clear();
}

double CompositeScorer::score(const FileRecord& file, const ScoringContext& ctx) const {
    if (file.is_pinned) return ctx.hot_threshold * 2;  // always above HOT threshold

    double total = 0.0;
    for (const auto& s : scorers_)
        total += s->score(file, ctx);
    return total;
}

std::string CompositeScorer::explain(const FileRecord& file) const {
    if (file.is_pinned)
        return "pinned=+30.0 (forced HOT)";

    std::ostringstream oss;
    double total = 0.0;
    for (const auto& s : scorers_) {
        double part = s->score(file, {});
        total += part;
        if (oss.tellp() > 0) oss << " ";
        oss << s->explain(file);
    }
    oss << " | total=" << (total >= 0 ? "+" : "")
        << std::fixed << std::setprecision(1) << total;
    return oss.str();
}

bool CompositeScorer::load_config(const nlohmann::json& config) {
    if (!config.is_array()) return false;
    clear_scorers();

    for (const auto& scorer_config : config) {
        std::string name = scorer_config.value("name", "");
        bool enabled = scorer_config.value("enabled", true);
        if (!enabled) continue;

        std::unique_ptr<Scorer> scorer;
        if (name == "recency")    scorer = std::make_unique<RecencyScorer>();
        else if (name == "frequency") scorer = std::make_unique<FrequencyScorer>();
        else if (name == "file_type") scorer = std::make_unique<TypeScorer>();
        else if (name == "size")       scorer = std::make_unique<SizeScorer>();
        else continue;

        scorer->load_config(scorer_config);
        scorers_.push_back(std::move(scorer));
    }

    return !scorers_.empty();
}
