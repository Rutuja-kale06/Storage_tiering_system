#include <gtest/gtest.h>
#include "catalog/persistent_catalog.hpp"
#include "core/file_record.hpp"
#include <filesystem>

namespace fs = std::filesystem;

class PersistentCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = "test_catalog.db";
        // Clean up from previous runs
        if (fs::exists(db_path_))
            fs::remove(db_path_);

        catalog_ = std::make_unique<PersistentCatalog>();
        ASSERT_TRUE(catalog_->init(db_path_));
    }

    void TearDown() override {
        catalog_->close();
        if (fs::exists(db_path_))
            fs::remove(db_path_);
    }

    FileRecord make_file(const std::string& id, const std::string& path,
                         Tier tier = Tier::HOT, int64_t size = 1024) {
        FileRecord f;
        f.id = id;
        f.path = path;
        f.extension = fs::path(path).extension().string();
        f.file_type = classify_extension(f.extension);
        f.current_tier = tier;
        f.target_tier = tier;
        f.size_bytes = size;
        f.access_count = 0;
        f.write_count = 0;
        f.created_at = time(nullptr);
        f.last_accessed = time(nullptr);
        f.last_modified = time(nullptr);
        f.migrate_count = 0;
        f.is_pinned = false;
        f.is_critical = (f.file_type == FileType::DATABASE);
        f.score = 0.0;
        return f;
    }

    std::string db_path_;
    std::unique_ptr<PersistentCatalog> catalog_;
};

TEST_F(PersistentCatalogTest, AddAndRetrieve) {
    auto f = make_file("test1", "/data/test.db");
    EXPECT_TRUE(catalog_->add_file(f));

    auto result = catalog_->get_file("test1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id, "test1");
    EXPECT_EQ(result->path, "/data/test.db");
    EXPECT_EQ(result->file_type, FileType::DATABASE);
    EXPECT_EQ(result->current_tier, Tier::HOT);
    EXPECT_EQ(result->size_bytes, 1024);
}

TEST_F(PersistentCatalogTest, GetByPath) {
    auto f = make_file("test2", "/data/file.log", Tier::WARM, 2048);
    f.file_type = FileType::LOGS;
    EXPECT_TRUE(catalog_->add_file(f));

    auto result = catalog_->get_file_by_path("/data/file.log");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id, "test2");
    EXPECT_EQ(result->current_tier, Tier::WARM);
    EXPECT_EQ(result->size_bytes, 2048);
}

TEST_F(PersistentCatalogTest, UpdateFile) {
    auto f = make_file("test3", "/data/update.me", Tier::COLD);
    EXPECT_TRUE(catalog_->add_file(f));

    f.current_tier = Tier::ARCHIVE;
    f.access_count = 42;
    f.is_pinned = true;
    EXPECT_TRUE(catalog_->update_file(f));

    auto result = catalog_->get_file("test3");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->current_tier, Tier::ARCHIVE);
    EXPECT_EQ(result->access_count, 42);
    EXPECT_TRUE(result->is_pinned);
}

TEST_F(PersistentCatalogTest, UpsertFile) {
    auto f = make_file("test4", "/data/upsert.me");
    EXPECT_TRUE(catalog_->upsert_file(f));  // insert

    f.size_bytes = 9999;
    EXPECT_TRUE(catalog_->upsert_file(f));  // update

    auto result = catalog_->get_file("test4");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size_bytes, 9999);
}

TEST_F(PersistentCatalogTest, DeleteFile) {
    auto f = make_file("test5", "/data/delete.me");
    EXPECT_TRUE(catalog_->add_file(f));
    EXPECT_TRUE(catalog_->delete_file("test5"));
    EXPECT_FALSE(catalog_->get_file("test5").has_value());
}

TEST_F(PersistentCatalogTest, RecordAccess) {
    auto f = make_file("test6", "/data/access.me");
    f.access_count = 10;
    f.last_accessed = time(nullptr) - 3600;  // 1 hour ago, so record_access will change it
    EXPECT_TRUE(catalog_->add_file(f));

    EXPECT_TRUE(catalog_->record_access("test6"));
    auto result = catalog_->get_file("test6");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->access_count, 11);
    EXPECT_NE(result->last_accessed, f.last_accessed);
}

TEST_F(PersistentCatalogTest, FilesByTier) {
    catalog_->add_file(make_file("a", "/a.txt", Tier::HOT));
    catalog_->add_file(make_file("b", "/b.log", Tier::WARM));
    catalog_->add_file(make_file("c", "/c.db", Tier::HOT));
    catalog_->add_file(make_file("d", "/d.mp4", Tier::COLD));

    auto hot = catalog_->files_by_tier(Tier::HOT);
    EXPECT_EQ(hot.size(), 2);

    auto warm = catalog_->files_by_tier(Tier::WARM);
    EXPECT_EQ(warm.size(), 1);

    auto cold = catalog_->files_by_tier(Tier::COLD);
    EXPECT_EQ(cold.size(), 1);
}

TEST_F(PersistentCatalogTest, TierStats) {
    catalog_->add_file(make_file("a", "/a.txt", Tier::HOT, 1024));
    catalog_->add_file(make_file("b", "/b.log", Tier::WARM, 2048));
    catalog_->add_file(make_file("c", "/c.db", Tier::HOT, 4096));

    auto all = catalog_->all_tier_stats();
    EXPECT_EQ(all[0].file_count, 2);  // HOT
    EXPECT_GT(all[0].total_bytes, 0);
    EXPECT_GT(all[0].monthly_cost_usd, 0);

    auto hot_stats = catalog_->stats_for_tier(Tier::HOT);
    EXPECT_EQ(hot_stats.file_count, 2);
}

TEST_F(PersistentCatalogTest, MigrationHistory) {
    // Add a file first so the foreign key constraint is satisfied
    auto f = make_file("test1", "/data/file.db");
    EXPECT_TRUE(catalog_->add_file(f));

    MigrationEvent e;
    e.file_id = "test1";
    e.file_path = "/data/file.db";
    e.from_tier = Tier::HOT;
    e.to_tier = Tier::COLD;
    e.size_bytes = 1024;
    e.reason = "test migration";
    e.timestamp = time(nullptr);
    e.success = true;
    e.duration_ms = 50.0;

    EXPECT_TRUE(catalog_->log_migration(e));
    EXPECT_TRUE(catalog_->log_migration(e));

    auto history = catalog_->recent_migrations(10);
    EXPECT_EQ(history.size(), 2);

    EXPECT_EQ(catalog_->total_migrations(), 2);
    EXPECT_EQ(catalog_->total_bytes_migrated(), 2048);
}

TEST_F(PersistentCatalogTest, BulkUpdateTier) {
    catalog_->add_file(make_file("a", "/a.txt", Tier::HOT));
    catalog_->add_file(make_file("b", "/b.txt", Tier::HOT));
    catalog_->add_file(make_file("c", "/c.txt", Tier::HOT));

    EXPECT_TRUE(catalog_->bulk_update_tier({"a", "b", "c"}, Tier::COLD));

    for (const auto& id : {"a", "b", "c"}) {
        auto f = catalog_->get_file(id);
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->current_tier, Tier::COLD);
    }
}

TEST_F(PersistentCatalogTest, FileCount) {
    EXPECT_EQ(catalog_->file_count(), 0);
    catalog_->add_file(make_file("a", "/a.txt"));
    EXPECT_EQ(catalog_->file_count(), 1);
    catalog_->add_file(make_file("b", "/b.txt"));
    EXPECT_EQ(catalog_->file_count(), 2);
}

TEST_F(PersistentCatalogTest, TotalBytes) {
    catalog_->add_file(make_file("a", "/a.txt", Tier::HOT, 1000));
    catalog_->add_file(make_file("b", "/b.txt", Tier::HOT, 2000));
    EXPECT_EQ(catalog_->total_bytes(), 3000);
}

TEST_F(PersistentCatalogTest, TotalMonthlyCost) {
    // HOT = $0.20/GB/month = $0.20/1073741824 bytes/month
    catalog_->add_file(make_file("a", "/a.txt", Tier::HOT, 1073741824));  // 1 GB
    double cost = catalog_->total_monthly_cost();
    EXPECT_NEAR(cost, 0.20 * 30, 0.01);  // $6.00/mo for 1GB HOT
}

TEST_F(PersistentCatalogTest, DuplicatePathRejected) {
    EXPECT_TRUE(catalog_->add_file(make_file("a", "/unique.txt")));
    // Same path should fail (UNIQUE constraint)
    auto f2 = make_file("b", "/unique.txt");
    EXPECT_FALSE(catalog_->add_file(f2));
}

TEST_F(PersistentCatalogTest, VacuumAndCheckpoint) {
    EXPECT_TRUE(catalog_->vacuum());
    EXPECT_TRUE(catalog_->checkpoint());
}

TEST_F(PersistentCatalogTest, ClearHistory) {
    auto f = make_file("a", "/a.txt");
    EXPECT_TRUE(catalog_->add_file(f));

    MigrationEvent e;
    e.file_id = "a"; e.file_path = "/a.txt";
    e.from_tier = Tier::HOT; e.to_tier = Tier::COLD;
    e.size_bytes = 100; e.timestamp = time(nullptr);
    e.success = true;

    catalog_->log_migration(e);
    EXPECT_EQ(catalog_->total_migrations(), 1);
    EXPECT_TRUE(catalog_->clear_history());
    EXPECT_EQ(catalog_->total_migrations(), 0);
}
