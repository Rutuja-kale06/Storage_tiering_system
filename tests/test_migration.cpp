#include <gtest/gtest.h>
#include <fstream>
#include "migration/local_migrator.hpp"
#include "migration/migration_planner.hpp"
#include "policy/policy_engine.hpp"
#include "core/file_record.hpp"
#include <filesystem>

namespace fs = std::filesystem;

class LocalMigratorTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = "test_migration_dir";
        if (fs::exists(test_dir_))
            fs::remove_all(test_dir_);
        fs::create_directories(test_dir_);

        migrator_ = std::make_unique<LocalMigrator>(test_dir_);
        migrator_->create_tier_directories();
    }

    void TearDown() override {
        if (fs::exists(test_dir_))
            fs::remove_all(test_dir_);
    }

    FileRecord make_test_file() {
        FileRecord f;
        f.id = "test_file_001";
        f.path = (fs::path(test_dir_) / "source.txt").string();
        f.extension = ".txt";
        f.file_type = FileType::OTHER;
        f.current_tier = Tier::HOT;
        f.target_tier = Tier::HOT;
        f.size_bytes = 1024;
        f.access_count = 0;
        f.created_at = time(nullptr);
        f.last_accessed = time(nullptr);
        f.last_modified = time(nullptr);
        f.migrate_count = 0;
        f.is_pinned = false;
        f.is_critical = false;
        f.score = 0.0;

        // Create actual file
        std::ofstream ofs(f.path);
        ofs << std::string(1024, 'X');
        ofs.close();

        return f;
    }

    std::string test_dir_;
    std::unique_ptr<LocalMigrator> migrator_;
};

TEST_F(LocalMigratorTest, CreateTierDirs) {
    EXPECT_TRUE(migrator_->create_tier_directories());
    for (const auto& name : {"HOT", "WARM", "COLD", "ARCHIVE"}) {
        EXPECT_TRUE(fs::exists(fs::path(test_dir_) / name));
    }
}

TEST_F(LocalMigratorTest, CanMigrate) {
    auto f = make_test_file();
    EXPECT_TRUE(migrator_->can_migrate(f, Tier::COLD));
}

TEST_F(LocalMigratorTest, MigrateToCold) {
    auto f = make_test_file();
    auto result = migrator_->migrate(f, Tier::COLD);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.new_path.empty());
    EXPECT_EQ(f.current_tier, Tier::COLD);
    EXPECT_TRUE(fs::exists(result.new_path));
    EXPECT_FALSE(fs::exists(fs::path(test_dir_) / "source.txt"));
}

TEST_F(LocalMigratorTest, VerifyAfterMigration) {
    auto f = make_test_file();
    migrator_->migrate(f, Tier::COLD);
    EXPECT_TRUE(migrator_->verify(f));
}

TEST_F(LocalMigratorTest, Rollback) {
    auto f = make_test_file();
    std::string original = f.path;

    auto result = migrator_->migrate(f, Tier::COLD);
    ASSERT_TRUE(result.success);

    EXPECT_TRUE(migrator_->rollback(f, original));
    EXPECT_TRUE(fs::exists(original));
    EXPECT_FALSE(fs::exists(result.new_path));
    EXPECT_EQ(f.current_tier, Tier::HOT);
}

// ── MigrationPlanner Tests ──────────────────────────────────

TEST(MigrationPlannerTest, EmptyPlan) {
    MigrationPlanner planner;
    std::vector<PolicyRecommendation> empty;
    auto plan = planner.plan(empty);
    EXPECT_EQ(plan.jobs.size(), 0);
}

TEST(MigrationPlannerTest, SingleJobPlan) {
    MigrationPlanner planner;
    PolicyRecommendation rec;
    rec.file.id = "test";
    rec.file.path = "/test.txt";
    rec.file.current_tier = Tier::HOT;
    rec.target_tier = Tier::COLD;
    rec.file.size_bytes = 1024;
    rec.estimated_monthly_savings = 0.50;
    rec.score = -10.0;
    rec.reason = "too cold";

    auto plan = planner.plan({rec});
    ASSERT_EQ(plan.jobs.size(), 1);
    EXPECT_EQ(plan.jobs[0].file_id, "test");
    EXPECT_EQ(plan.jobs[0].from_tier, Tier::HOT);
    EXPECT_EQ(plan.jobs[0].to_tier, Tier::COLD);
    EXPECT_GT(plan.total_monthly_savings, 0);
}

TEST(MigrationPlannerTest, BatchJobs) {
    MigrationPlanner planner;
    std::vector<PolicyRecommendation> recs;
    for (int i = 0; i < 10; ++i) {
        PolicyRecommendation rec;
        rec.file.id = "f" + std::to_string(i);
        rec.file.current_tier = Tier::HOT;
        rec.target_tier = Tier::COLD;
        rec.file.size_bytes = 1024;
        rec.estimated_monthly_savings = 0.10;
        recs.push_back(rec);
    }

    auto plan = planner.plan(recs);
    auto batches = planner.batch_jobs(plan);

    EXPECT_GT(batches.size(), 0);
    int total = 0;
    for (const auto& batch : batches) total += batch.size();
    EXPECT_EQ(total, 10);
}

TEST(MigrationPlannerTest, SortedBySavings) {
    MigrationPlanner planner;
    std::vector<PolicyRecommendation> recs;

    for (int i = 0; i < 5; ++i) {
        PolicyRecommendation rec;
        rec.file.id = "f" + std::to_string(i);
        rec.file.current_tier = Tier::HOT;
        rec.target_tier = Tier::COLD;
        rec.file.size_bytes = 1024 * (i + 1);
        rec.estimated_monthly_savings = 0.10 * (i + 1);
        recs.push_back(rec);
    }

    auto plan = planner.plan(recs);
    ASSERT_EQ(plan.jobs.size(), 5);

    // Should be sorted by savings descending
    for (size_t i = 1; i < plan.jobs.size(); ++i) {
        EXPECT_GE(plan.jobs[i - 1].estimated_savings_per_month,
                  plan.jobs[i].estimated_savings_per_month);
    }
}

TEST(MigrationPlannerTest, MaintenanceWindow) {
    MigrationPlanner planner;
    planner.constraints().window_enabled = true;
    planner.constraints().window_start_hour = 2;
    planner.constraints().window_end_hour = 5;

    // Can't control time() in test, but can verify it doesn't crash
    EXPECT_NO_THROW(planner.within_maintenance_window());
}
