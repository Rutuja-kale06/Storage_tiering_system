#include <gtest/gtest.h>
#include "policy/policy_engine.hpp"
#include "policy/recency_scorer.hpp"
#include "policy/frequency_scorer.hpp"
#include "policy/type_scorer.hpp"
#include "policy/size_scorer.hpp"
#include "policy/composite_scorer.hpp"
#include "core/file_record.hpp"

class PolicyTest : public ::testing::Test {
protected:
    FileRecord make_test_file(const std::string& id, Tier tier = Tier::HOT,
                              FileType ft = FileType::OTHER,
                              int64_t size = 1024,
                              int access_count = 0,
                              time_t last_access_offset = 0) {
        FileRecord f;
        f.id = id;
        f.path = "/test/" + id;
        f.extension = ".bin";
        f.file_type = ft;
        f.current_tier = tier;
        f.target_tier = tier;
        f.size_bytes = size;
        f.access_count = access_count;
        f.write_count = 0;
        f.created_at = time(nullptr) - 86400 * 100;  // 100 days old
        f.last_accessed = time(nullptr) - last_access_offset;
        f.last_modified = f.last_accessed;
        f.migrate_count = 0;
        f.is_pinned = false;
        f.is_critical = false;
        f.score = 0.0;
        return f;
    }
};

TEST_F(PolicyTest, RecencyScorerHot) {
    RecencyScorer scorer;
    auto f = make_test_file("hot", Tier::HOT, FileType::OTHER, 1024, 10, 0);  // accessed now
    double s = scorer.score(f, {});
    EXPECT_GE(s, 15.0);  // should be very hot (idle < 1 day = +20)
}

TEST_F(PolicyTest, RecencyScorerCold) {
    RecencyScorer scorer;
    auto f = make_test_file("cold", Tier::ARCHIVE, FileType::OTHER, 1024, 0, 86400 * 60);  // 60 days idle
    double s = scorer.score(f, {});
    EXPECT_LE(s, -10.0);  // should be cold (idle > 30 days = -22)
}

TEST_F(PolicyTest, FrequencyScorer) {
    FrequencyScorer scorer;
    auto f = make_test_file("freq", Tier::HOT, FileType::OTHER, 1024, 100, 0);
    // 100 accesses / 100 days old = 1.0 rate
    // 1.0 * 3.0 = 3.0, capped at 12.0
    double s = scorer.score(f, {});
    EXPECT_NEAR(s, 3.0, 0.5);
}

TEST_F(PolicyTest, TypeScorerDatabase) {
    TypeScorer scorer;
    auto f = make_test_file("db", Tier::HOT, FileType::DATABASE);
    double s = scorer.score(f, {});
    EXPECT_EQ(s, 15.0);
}

TEST_F(PolicyTest, TypeScorerTemporary) {
    TypeScorer scorer;
    auto f = make_test_file("tmp", Tier::HOT, FileType::TEMPORARY);
    double s = scorer.score(f, {});
    EXPECT_EQ(s, -15.0);
}

TEST_F(PolicyTest, SizeScorerHuge) {
    SizeScorer scorer;
    auto f = make_test_file("huge", Tier::HOT, FileType::OTHER, 30LL * 1024 * 1024 * 1024);  // 30 GB
    double s = scorer.score(f, {});
    EXPECT_EQ(s, -10.0);
}

TEST_F(PolicyTest, SizeScorerLarge) {
    SizeScorer scorer;
    auto f = make_test_file("large", Tier::HOT, FileType::OTHER, 10LL * 1024 * 1024 * 1024);  // 10 GB
    double s = scorer.score(f, {});
    EXPECT_EQ(s, -5.0);
}

TEST_F(PolicyTest, CompositeScorerPinned) {
    CompositeScorer scorer;
    auto f = make_test_file("pinned", Tier::HOT);
    f.is_pinned = true;
    double s = scorer.score(f, {.hot_threshold = 15});
    EXPECT_GE(s, 30.0);  // pinned = always hot
}

TEST_F(PolicyTest, TargetTierMapping) {
    PolicyEngine engine;

    // Hot file: DATABASE (+15), accessed now (recency=+20) → ~35 > 15
    auto hot_file = make_test_file("hot", Tier::HOT, FileType::DATABASE, 1024, 10, 0);
    EXPECT_EQ(engine.target_tier(hot_file), Tier::HOT);

    // Warm file: ANALYTICS (+5), idle 2d (recency=+12), not large → ~17 - 5(age) = 12 >= 0
    auto warm_file = make_test_file("warm", Tier::WARM, FileType::ANALYTICS, 1024, 5, 86400 * 2);
    EXPECT_EQ(engine.target_tier(warm_file), Tier::WARM);

    // Cold file: OTHER (0), idle 10d (recency=-5), small → -5 - 5(age) = -10 >= -15
    auto cold_file = make_test_file("cold", Tier::COLD, FileType::OTHER, 1024, 0, 86400 * 10);
    EXPECT_EQ(engine.target_tier(cold_file), Tier::COLD);

    // Archive file: BACKUP (-10), idle 60d (recency=-22), huge 50GB (-10) → -42 -5(age) = -47 < -15
    auto archive_file = make_test_file("arch", Tier::ARCHIVE, FileType::BACKUP,
                                         50LL * 1024 * 1024 * 1024, 0, 86400 * 200);
    EXPECT_EQ(engine.target_tier(archive_file), Tier::ARCHIVE);
}

TEST_F(PolicyTest, ExplainScore) {
    PolicyEngine engine;
    auto f = make_test_file("test", Tier::HOT, FileType::DATABASE, 1024, 5, 3600);
    std::string explanation = engine.explain_score(f);
    EXPECT_FALSE(explanation.empty());
    EXPECT_NE(explanation.find("recency"), std::string::npos);
    EXPECT_NE(explanation.find("freq"), std::string::npos);
    EXPECT_NE(explanation.find("type"), std::string::npos);
}

TEST_F(PolicyTest, CompositeScorerOrder) {
    CompositeScorer scorer;
    // Order should be: recency, frequency, type, size
    EXPECT_EQ(scorer.scorers().size(), 4);
    EXPECT_EQ(scorer.scorers()[0]->name(), "recency");
    EXPECT_EQ(scorer.scorers()[1]->name(), "frequency");
    EXPECT_EQ(scorer.scorers()[2]->name(), "file_type");
    EXPECT_EQ(scorer.scorers()[3]->name(), "size");
}

TEST_F(PolicyTest, CriticalFlagBonus) {
    PolicyEngine engine;
    auto f = make_test_file("crit", Tier::HOT, FileType::OTHER, 1024, 0, 86400 * 100);
    f.is_critical = true;
    double s_with = engine.score_file(f);

    f.is_critical = false;
    double s_without = engine.score_file(f);

    EXPECT_GT(s_with, s_without);
}

TEST_F(PolicyTest, AgePenalty) {
    PolicyEngine engine;
    // File older than 90 days
    auto old_file = make_test_file("old", Tier::HOT, FileType::OTHER, 1024, 0, 86400 * 200);
    old_file.created_at = time(nullptr) - 86400 * 200;
    double s = engine.score_file(old_file);
    EXPECT_LT(s, 0);  // age penalty should make it negative
}
