#include "catalog/persistent_catalog.hpp"
#include "core/types.hpp"
#include <stdexcept>
#include <cstring>

PersistentCatalog::PersistentCatalog() = default;

PersistentCatalog::~PersistentCatalog() {
    close();
}

bool PersistentCatalog::init(const std::string& db_path) {
    std::lock_guard<std::mutex> lk(mtx_);

    if (initialized_.load()) {
        close();
    }

    int rc = sqlite3_open_v2(
        db_path.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (rc != SQLITE_OK) {
        db_ = nullptr;
        return false;
    }

    // Enable WAL mode and foreign keys
    if (!exec_sql(DB_SCHEMA)) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    if (!verify_schema()) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    if (!prepare_statements()) {
        finalize_statements();
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    initialized_.store(true);
    return true;
}

void PersistentCatalog::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (db_) {
        finalize_statements();
        sqlite3_close(db_);
        db_ = nullptr;
    }
    initialized_.store(false);
}

bool PersistentCatalog::prepare_statements() {
    const char* sql;

    sql = "INSERT INTO files (id,path,extension,file_type,current_tier,target_tier,"
          "size_bytes,access_count,write_count,created_at,last_accessed,last_modified,"
          "migrate_count,is_pinned,is_critical,score,owner_id,"
          "s3_bucket,s3_key,content_type,etag) "
          "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,"
          "?18,?19,?20,?21)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_add_file_, nullptr) != SQLITE_OK)
        return false;

    sql = "UPDATE files SET path=?2,extension=?3,file_type=?4,current_tier=?5,"
          "target_tier=?6,size_bytes=?7,access_count=?8,write_count=?9,"
          "created_at=?10,last_accessed=?11,last_modified=?12,migrate_count=?13,"
          "is_pinned=?14,is_critical=?15,score=?16,owner_id=?17,"
          "s3_bucket=?18,s3_key=?19,content_type=?20,etag=?21 WHERE id=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_update_file_, nullptr) != SQLITE_OK)
        return false;

    sql = "INSERT INTO files (id,path,extension,file_type,current_tier,target_tier,"
          "size_bytes,access_count,write_count,created_at,last_accessed,last_modified,"
          "migrate_count,is_pinned,is_critical,score,owner_id,"
          "s3_bucket,s3_key,content_type,etag) "
          "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,"
          "?18,?19,?20,?21) "
          "ON CONFLICT(id) DO UPDATE SET "
          "path=excluded.path, extension=excluded.extension, "
          "file_type=excluded.file_type, current_tier=excluded.current_tier, "
          "target_tier=excluded.target_tier, size_bytes=excluded.size_bytes, "
          "access_count=excluded.access_count, write_count=excluded.write_count, "
          "created_at=excluded.created_at, last_accessed=excluded.last_accessed, "
          "last_modified=excluded.last_modified, migrate_count=excluded.migrate_count, "
          "is_pinned=excluded.is_pinned, is_critical=excluded.is_critical, "
          "score=excluded.score, owner_id=excluded.owner_id, "
          "s3_bucket=excluded.s3_bucket, s3_key=excluded.s3_key, "
          "content_type=excluded.content_type, etag=excluded.etag";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_upsert_file_, nullptr) != SQLITE_OK)
        return false;

    sql = "SELECT id,path,extension,file_type,current_tier,target_tier,"
          "size_bytes,access_count,write_count,created_at,last_accessed,last_modified,"
          "migrate_count,is_pinned,is_critical,score,owner_id,"
          "s3_bucket,s3_key,content_type,etag FROM files WHERE id=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_get_file_, nullptr) != SQLITE_OK)
        return false;

    sql = "SELECT id,path,extension,file_type,current_tier,target_tier,"
          "size_bytes,access_count,write_count,created_at,last_accessed,last_modified,"
          "migrate_count,is_pinned,is_critical,score,owner_id,"
          "s3_bucket,s3_key,content_type,etag FROM files WHERE path=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_get_file_by_path_, nullptr) != SQLITE_OK)
        return false;

    sql = "DELETE FROM files WHERE id=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_delete_file_, nullptr) != SQLITE_OK)
        return false;

    sql = "UPDATE files SET access_count=access_count+1, last_accessed=?1 WHERE id=?2";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_record_access_, nullptr) != SQLITE_OK)
        return false;

    sql = "INSERT INTO migration_history (file_id,file_path,from_tier,to_tier,"
          "size_bytes,reason,timestamp,success,duration_ms) "
          "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_log_migration_, nullptr) != SQLITE_OK)
        return false;

    // Extent prepared statements
    sql = "INSERT INTO extents (id,volume_id,offset_bytes,size_bytes,current_tier,target_tier,"
          "heat_score,prev_heat_score,iops_read,iops_write,bytes_read,bytes_written,"
          "sequential_ratio,pe_cycles,is_pinned,sample_count,last_sampled,last_migrated,"
          "physical_path,physical_offset) "
          "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_add_extent_, nullptr) != SQLITE_OK)
        return false;

    sql = "UPDATE extents SET volume_id=?2,offset_bytes=?3,size_bytes=?4,current_tier=?5,"
          "target_tier=?6,heat_score=?7,prev_heat_score=?8,iops_read=?9,iops_write=?10,"
          "bytes_read=?11,bytes_written=?12,sequential_ratio=?13,pe_cycles=?14,is_pinned=?15,"
          "sample_count=?16,last_sampled=?17,last_migrated=?18,physical_path=?19,"
          "physical_offset=?20 WHERE id=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_update_extent_, nullptr) != SQLITE_OK)
        return false;

    sql = "INSERT INTO extents (id,volume_id,offset_bytes,size_bytes,current_tier,target_tier,"
          "heat_score,prev_heat_score,iops_read,iops_write,bytes_read,bytes_written,"
          "sequential_ratio,pe_cycles,is_pinned,sample_count,last_sampled,last_migrated,"
          "physical_path,physical_offset) "
          "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20) "
          "ON CONFLICT(id) DO UPDATE SET "
          "volume_id=excluded.volume_id, offset_bytes=excluded.offset_bytes, "
          "size_bytes=excluded.size_bytes, current_tier=excluded.current_tier, "
          "target_tier=excluded.target_tier, heat_score=excluded.heat_score, "
          "prev_heat_score=excluded.prev_heat_score, iops_read=excluded.iops_read, "
          "iops_write=excluded.iops_write, bytes_read=excluded.bytes_read, "
          "bytes_written=excluded.bytes_written, sequential_ratio=excluded.sequential_ratio, "
          "pe_cycles=excluded.pe_cycles, is_pinned=excluded.is_pinned, "
          "sample_count=excluded.sample_count, last_sampled=excluded.last_sampled, "
          "last_migrated=excluded.last_migrated, physical_path=excluded.physical_path, "
          "physical_offset=excluded.physical_offset";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_upsert_extent_, nullptr) != SQLITE_OK)
        return false;

    sql = "SELECT id,volume_id,offset_bytes,size_bytes,current_tier,target_tier,"
          "heat_score,prev_heat_score,iops_read,iops_write,bytes_read,bytes_written,"
          "sequential_ratio,pe_cycles,is_pinned,sample_count,last_sampled,last_migrated,"
          "physical_path,physical_offset FROM extents WHERE id=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_get_extent_, nullptr) != SQLITE_OK)
        return false;

    sql = "SELECT id,volume_id,offset_bytes,size_bytes,current_tier,target_tier,"
          "heat_score,prev_heat_score,iops_read,iops_write,bytes_read,bytes_written,"
          "sequential_ratio,pe_cycles,is_pinned,sample_count,last_sampled,last_migrated,"
          "physical_path,physical_offset FROM extents WHERE volume_id=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_extents_by_volume_, nullptr) != SQLITE_OK)
        return false;

    sql = "SELECT id,volume_id,offset_bytes,size_bytes,current_tier,target_tier,"
          "heat_score,prev_heat_score,iops_read,iops_write,bytes_read,bytes_written,"
          "sequential_ratio,pe_cycles,is_pinned,sample_count,last_sampled,last_migrated,"
          "physical_path,physical_offset FROM extents WHERE current_tier=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_extents_by_tier_, nullptr) != SQLITE_OK)
        return false;

    sql = "SELECT id,volume_id,offset_bytes,size_bytes,current_tier,target_tier,"
          "heat_score,prev_heat_score,iops_read,iops_write,bytes_read,bytes_written,"
          "sequential_ratio,pe_cycles,is_pinned,sample_count,last_sampled,last_migrated,"
          "physical_path,physical_offset FROM extents";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_all_extents_, nullptr) != SQLITE_OK)
        return false;

    sql = "DELETE FROM extents WHERE id=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_delete_extent_, nullptr) != SQLITE_OK)
        return false;

    sql = "INSERT INTO io_samples (extent_id,timestamp,iops_read,iops_write,"
          "bytes_read,bytes_written,latency_us) "
          "VALUES (?1,?2,?3,?4,?5,?6,?7)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_log_io_sample_, nullptr) != SQLITE_OK)
        return false;

    sql = "INSERT INTO heat_map_history (extent_id,timestamp,heat_score,tier) "
          "VALUES (?1,?2,?3,?4)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_log_heat_snapshot_, nullptr) != SQLITE_OK)
        return false;

    sql = "INSERT INTO extent_migration_history (extent_id,volume_id,from_tier,to_tier,"
          "size_bytes,reason,timestamp,success,duration_ms) "
          "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_log_extent_migrate_, nullptr) != SQLITE_OK)
        return false;

    sql = "INSERT INTO volumes (id,path,total_size,extent_size,label,is_active,registered_at) "
          "VALUES (?1,?2,?3,?4,?5,?6,?7)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_add_volume_, nullptr) != SQLITE_OK)
        return false;

    sql = "SELECT id,path,total_size,extent_size,label,is_active,registered_at "
          "FROM volumes WHERE id=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_get_volume_, nullptr) != SQLITE_OK)
        return false;

    sql = "SELECT id,path,total_size,extent_size,label,is_active,registered_at FROM volumes";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_all_volumes_, nullptr) != SQLITE_OK)
        return false;

    sql = "DELETE FROM volumes WHERE id=?1";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_delete_volume_, nullptr) != SQLITE_OK)
        return false;

    return true;
}

void PersistentCatalog::finalize_statements() {
    auto finalize = [](sqlite3_stmt*& stmt) {
        if (stmt) { sqlite3_finalize(stmt); stmt = nullptr; }
    };
    finalize(stmt_add_file_);
    finalize(stmt_update_file_);
    finalize(stmt_upsert_file_);
    finalize(stmt_get_file_);
    finalize(stmt_get_file_by_path_);
    finalize(stmt_delete_file_);
    finalize(stmt_record_access_);
    finalize(stmt_log_migration_);

    finalize(stmt_add_extent_);
    finalize(stmt_update_extent_);
    finalize(stmt_upsert_extent_);
    finalize(stmt_get_extent_);
    finalize(stmt_extents_by_volume_);
    finalize(stmt_extents_by_tier_);
    finalize(stmt_all_extents_);
    finalize(stmt_delete_extent_);
    finalize(stmt_log_io_sample_);
    finalize(stmt_log_heat_snapshot_);
    finalize(stmt_log_extent_migrate_);

    finalize(stmt_add_volume_);
    finalize(stmt_get_volume_);
    finalize(stmt_all_volumes_);
    finalize(stmt_delete_volume_);
}

bool PersistentCatalog::exec_sql(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool PersistentCatalog::verify_schema() {
    const char* sql = "SELECT name FROM sqlite_master WHERE type='table' "
                      "AND name IN ('files','migration_history','metadata')";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    std::vector<std::string> tables;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        tables.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);

    for (const auto& t : {"files", "migration_history", "metadata"}) {
        if (std::find(tables.begin(), tables.end(), t) == tables.end())
            return false;
    }

    // Migrate existing databases: add S3 columns if missing
    const std::vector<std::pair<std::string, std::string>> s3_cols = {
        {"s3_bucket", "TEXT DEFAULT ''"},
        {"s3_key", "TEXT DEFAULT ''"},
        {"content_type", "TEXT DEFAULT ''"},
        {"etag", "TEXT DEFAULT ''"}
    };
    for (const auto& [col_name, col_def] : s3_cols) {
        std::string alt = "ALTER TABLE files ADD COLUMN " + col_name + " " + col_def;
        sqlite3_exec(db_, alt.c_str(), nullptr, nullptr, nullptr);
    }

    // Create indexes for S3 lookups (IF NOT EXISTS is safe to re-run)
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_files_s3_bucket ON files(s3_bucket)", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_files_s3_key ON files(s3_key)", nullptr, nullptr, nullptr);

    // Migrate existing databases: add owner_id column if missing
    sqlite3_exec(db_, "ALTER TABLE files ADD COLUMN owner_id TEXT DEFAULT ''", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_files_owner ON files(owner_id)", nullptr, nullptr, nullptr);

    return true;
}

FileRecord PersistentCatalog::row_to_file_record(sqlite3_stmt* stmt) const {
    FileRecord f;
    f.id            = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    f.path          = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    f.extension     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    f.file_type     = static_cast<FileType>(sqlite3_column_int(stmt, 3));
    f.current_tier  = static_cast<Tier>(sqlite3_column_int(stmt, 4));
    f.target_tier   = static_cast<Tier>(sqlite3_column_int(stmt, 5));
    f.size_bytes    = sqlite3_column_int64(stmt, 6);
    f.access_count  = sqlite3_column_int(stmt, 7);
    f.write_count   = sqlite3_column_int(stmt, 8);
    f.created_at    = sqlite3_column_int64(stmt, 9);
    f.last_accessed = sqlite3_column_int64(stmt, 10);
    f.last_modified = sqlite3_column_int64(stmt, 11);
    f.migrate_count = sqlite3_column_int(stmt, 12);
    f.is_pinned     = sqlite3_column_int(stmt, 13) != 0;
    f.is_critical   = sqlite3_column_int(stmt, 14) != 0;
    f.score         = sqlite3_column_double(stmt, 15);
    f.owner_id      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
    f.s3_bucket     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 17));
    f.s3_key        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 18));
    f.content_type  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 19));
    f.etag          = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 20));
    return f;
}

ExtentRecord PersistentCatalog::row_to_extent(sqlite3_stmt* stmt) const {
    ExtentRecord e;
    e.id              = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    e.volume_id       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    e.offset_bytes    = sqlite3_column_int64(stmt, 2);
    e.size_bytes      = sqlite3_column_int64(stmt, 3);
    e.current_tier    = static_cast<Tier>(sqlite3_column_int(stmt, 4));
    e.target_tier     = static_cast<Tier>(sqlite3_column_int(stmt, 5));
    e.heat_score      = sqlite3_column_double(stmt, 6);
    e.prev_heat_score = sqlite3_column_double(stmt, 7);
    e.iops_read       = sqlite3_column_int64(stmt, 8);
    e.iops_write      = sqlite3_column_int64(stmt, 9);
    e.bytes_read      = sqlite3_column_int64(stmt, 10);
    e.bytes_written   = sqlite3_column_int64(stmt, 11);
    e.sequential_ratio= sqlite3_column_double(stmt, 12);
    e.pe_cycles       = sqlite3_column_int64(stmt, 13);
    e.is_pinned       = sqlite3_column_int(stmt, 14) != 0;
    e.sample_count    = sqlite3_column_int(stmt, 15);
    e.last_sampled    = sqlite3_column_int64(stmt, 16);
    e.last_migrated   = sqlite3_column_int64(stmt, 17);
    e.physical_path   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 18));
    e.physical_offset = sqlite3_column_int64(stmt, 19);
    return e;
}

VolumeRecord PersistentCatalog::row_to_volume(sqlite3_stmt* stmt) const {
    VolumeRecord v;
    v.id            = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    v.path          = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    v.total_size    = sqlite3_column_int64(stmt, 2);
    v.extent_size   = sqlite3_column_int64(stmt, 3);
    v.label         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    v.is_active     = sqlite3_column_int(stmt, 5) != 0;
    v.registered_at = sqlite3_column_int64(stmt, 6);
    return v;
}

bool PersistentCatalog::bind_extent(sqlite3_stmt* stmt, const ExtentRecord& e) {
    sqlite3_bind_text(stmt, 1, e.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, e.volume_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, e.offset_bytes);
    sqlite3_bind_int64(stmt, 4, e.size_bytes);
    sqlite3_bind_int(stmt, 5, static_cast<int>(e.current_tier));
    sqlite3_bind_int(stmt, 6, static_cast<int>(e.target_tier));
    sqlite3_bind_double(stmt, 7, e.heat_score);
    sqlite3_bind_double(stmt, 8, e.prev_heat_score);
    sqlite3_bind_int64(stmt, 9, e.iops_read);
    sqlite3_bind_int64(stmt, 10, e.iops_write);
    sqlite3_bind_int64(stmt, 11, e.bytes_read);
    sqlite3_bind_int64(stmt, 12, e.bytes_written);
    sqlite3_bind_double(stmt, 13, e.sequential_ratio);
    sqlite3_bind_int64(stmt, 14, e.pe_cycles);
    sqlite3_bind_int(stmt, 15, e.is_pinned ? 1 : 0);
    sqlite3_bind_int(stmt, 16, e.sample_count);
    sqlite3_bind_int64(stmt, 17, static_cast<int64_t>(e.last_sampled));
    sqlite3_bind_int64(stmt, 18, static_cast<int64_t>(e.last_migrated));
    sqlite3_bind_text(stmt, 19, e.physical_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 20, e.physical_offset);
    return true;
}

// ── Extent CRUD ────────────────────────────────────────────────────

bool PersistentCatalog::add_extent(const ExtentRecord& extent) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_add_extent_);
    bind_extent(stmt_add_extent_, extent);
    return sqlite3_step(stmt_add_extent_) == SQLITE_DONE;
}

bool PersistentCatalog::update_extent(const ExtentRecord& extent) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_update_extent_);
    bind_extent(stmt_update_extent_, extent);
    return sqlite3_step(stmt_update_extent_) == SQLITE_DONE;
}

bool PersistentCatalog::upsert_extent(const ExtentRecord& extent) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_upsert_extent_);
    bind_extent(stmt_upsert_extent_, extent);
    return sqlite3_step(stmt_upsert_extent_) == SQLITE_DONE;
}

std::optional<ExtentRecord> PersistentCatalog::get_extent(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_get_extent_);
    sqlite3_bind_text(stmt_get_extent_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_get_extent_) == SQLITE_ROW)
        return row_to_extent(stmt_get_extent_);
    return std::nullopt;
}

std::vector<ExtentRecord> PersistentCatalog::extents_by_volume(
    const std::string& volume_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ExtentRecord> result;
    sqlite3_reset(stmt_extents_by_volume_);
    sqlite3_bind_text(stmt_extents_by_volume_, 1, volume_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt_extents_by_volume_) == SQLITE_ROW)
        result.push_back(row_to_extent(stmt_extents_by_volume_));
    return result;
}

std::vector<ExtentRecord> PersistentCatalog::extents_by_tier(Tier t) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ExtentRecord> result;
    sqlite3_reset(stmt_extents_by_tier_);
    sqlite3_bind_int(stmt_extents_by_tier_, 1, static_cast<int>(t));
    while (sqlite3_step(stmt_extents_by_tier_) == SQLITE_ROW)
        result.push_back(row_to_extent(stmt_extents_by_tier_));
    return result;
}

std::vector<ExtentRecord> PersistentCatalog::all_extents() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ExtentRecord> result;
    sqlite3_reset(stmt_all_extents_);
    while (sqlite3_step(stmt_all_extents_) == SQLITE_ROW)
        result.push_back(row_to_extent(stmt_all_extents_));
    return result;
}

int PersistentCatalog::extent_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "SELECT COUNT(*) FROM extents";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int count = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return count;
}

int PersistentCatalog::extent_count_by_tier(Tier t) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "SELECT COUNT(*) FROM extents WHERE current_tier=?1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, static_cast<int>(t));
    int count = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return count;
}

bool PersistentCatalog::delete_extent(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_delete_extent_);
    sqlite3_bind_text(stmt_delete_extent_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt_delete_extent_) == SQLITE_DONE;
}

// ── IO Samples ────────────────────────────────────────────────────

bool PersistentCatalog::log_io_sample(const std::string& extent_id, time_t timestamp,
                                       int64_t iops_read, int64_t iops_write,
                                       int64_t bytes_read, int64_t bytes_written,
                                       double latency_us) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_log_io_sample_);
    sqlite3_bind_text(stmt_log_io_sample_, 1, extent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_log_io_sample_, 2, static_cast<int64_t>(timestamp));
    sqlite3_bind_int64(stmt_log_io_sample_, 3, iops_read);
    sqlite3_bind_int64(stmt_log_io_sample_, 4, iops_write);
    sqlite3_bind_int64(stmt_log_io_sample_, 5, bytes_read);
    sqlite3_bind_int64(stmt_log_io_sample_, 6, bytes_written);
    sqlite3_bind_double(stmt_log_io_sample_, 7, latency_us);
    return sqlite3_step(stmt_log_io_sample_) == SQLITE_DONE;
}

std::vector<IOSample> PersistentCatalog::recent_io_samples(
    const std::string& extent_id, int limit) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<IOSample> result;
    const char* sql = "SELECT timestamp,iops_read,iops_write,bytes_read,bytes_written,latency_us "
                      "FROM io_samples WHERE extent_id=?1 ORDER BY id DESC LIMIT ?2";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, extent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        IOSample s;
        s.timestamp     = sqlite3_column_int64(stmt, 0);
        s.iops_read     = sqlite3_column_int64(stmt, 1);
        s.iops_write    = sqlite3_column_int64(stmt, 2);
        s.bytes_read    = sqlite3_column_int64(stmt, 3);
        s.bytes_written = sqlite3_column_int64(stmt, 4);
        s.latency_us    = sqlite3_column_double(stmt, 5);
        result.push_back(s);
    }
    sqlite3_finalize(stmt);
    return result;
}

// ── Heat Map History ──────────────────────────────────────────────

bool PersistentCatalog::log_heat_snapshot(const std::string& extent_id, time_t timestamp,
                                           double heat_score, Tier tier) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_log_heat_snapshot_);
    sqlite3_bind_text(stmt_log_heat_snapshot_, 1, extent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_log_heat_snapshot_, 2, static_cast<int64_t>(timestamp));
    sqlite3_bind_double(stmt_log_heat_snapshot_, 3, heat_score);
    sqlite3_bind_int(stmt_log_heat_snapshot_, 4, static_cast<int>(tier));
    return sqlite3_step(stmt_log_heat_snapshot_) == SQLITE_DONE;
}

std::vector<std::tuple<time_t, double, Tier>> PersistentCatalog::heat_history(
    const std::string& extent_id, int hours) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::tuple<time_t, double, Tier>> result;
    const char* sql = "SELECT timestamp,heat_score,tier FROM heat_map_history "
                      "WHERE extent_id=?1 AND timestamp >= ?2 ORDER BY timestamp ASC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, extent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(std::time(nullptr) - hours * 3600));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto ts = sqlite3_column_int64(stmt, 0);
        double heat = sqlite3_column_double(stmt, 1);
        Tier t = static_cast<Tier>(sqlite3_column_int(stmt, 2));
        result.emplace_back(ts, heat, t);
    }
    sqlite3_finalize(stmt);
    return result;
}

// ── Extent Migration History ──────────────────────────────────────

bool PersistentCatalog::log_extent_migration(const std::string& extent_id,
                                              const std::string& volume_id,
                                              Tier from_tier, Tier to_tier,
                                              int64_t size_bytes,
                                              const std::string& reason) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_log_extent_migrate_);
    sqlite3_bind_text(stmt_log_extent_migrate_, 1, extent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_log_extent_migrate_, 2, volume_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_log_extent_migrate_, 3, static_cast<int>(from_tier));
    sqlite3_bind_int(stmt_log_extent_migrate_, 4, static_cast<int>(to_tier));
    sqlite3_bind_int64(stmt_log_extent_migrate_, 5, size_bytes);
    sqlite3_bind_text(stmt_log_extent_migrate_, 6, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_log_extent_migrate_, 7, static_cast<int64_t>(std::time(nullptr)));
    sqlite3_bind_int(stmt_log_extent_migrate_, 8, 1);
    sqlite3_bind_double(stmt_log_extent_migrate_, 9, 0.0);
    return sqlite3_step(stmt_log_extent_migrate_) == SQLITE_DONE;
}

std::vector<MigrationEvent> PersistentCatalog::recent_extent_migrations(int n) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<MigrationEvent> result;
    const char* sql = "SELECT extent_id,volume_id,from_tier,to_tier,size_bytes,"
                      "COALESCE(reason,''),timestamp,success,duration_ms "
                      "FROM extent_migration_history ORDER BY id DESC LIMIT ?1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_int(stmt, 1, n);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MigrationEvent e;
        e.file_id    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        e.file_path  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        e.from_tier  = static_cast<Tier>(sqlite3_column_int(stmt, 2));
        e.to_tier    = static_cast<Tier>(sqlite3_column_int(stmt, 3));
        e.size_bytes = sqlite3_column_int64(stmt, 4);
        e.reason     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        e.timestamp  = sqlite3_column_int64(stmt, 6);
        e.success    = sqlite3_column_int(stmt, 7) != 0;
        e.duration_ms= sqlite3_column_double(stmt, 8);
        result.push_back(e);
    }
    sqlite3_finalize(stmt);
    return result;
}

// ── Volume CRUD ───────────────────────────────────────────────────

bool PersistentCatalog::add_volume(const VolumeRecord& vol) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_add_volume_);
    sqlite3_bind_text(stmt_add_volume_, 1, vol.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_add_volume_, 2, vol.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_add_volume_, 3, vol.total_size);
    sqlite3_bind_int64(stmt_add_volume_, 4, vol.extent_size);
    sqlite3_bind_text(stmt_add_volume_, 5, vol.label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_add_volume_, 6, vol.is_active ? 1 : 0);
    sqlite3_bind_int64(stmt_add_volume_, 7, static_cast<int64_t>(vol.registered_at));
    return sqlite3_step(stmt_add_volume_) == SQLITE_DONE;
}

bool PersistentCatalog::update_volume(const VolumeRecord& vol) {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "UPDATE volumes SET path=?2,total_size=?3,extent_size=?4,"
                      "label=?5,is_active=?6 WHERE id=?1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, vol.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, vol.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, vol.total_size);
    sqlite3_bind_int64(stmt, 4, vol.extent_size);
    sqlite3_bind_text(stmt, 5, vol.label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, vol.is_active ? 1 : 0);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<VolumeRecord> PersistentCatalog::get_volume(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_get_volume_);
    sqlite3_bind_text(stmt_get_volume_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_get_volume_) == SQLITE_ROW)
        return row_to_volume(stmt_get_volume_);
    return std::nullopt;
}

std::vector<VolumeRecord> PersistentCatalog::all_volumes() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<VolumeRecord> result;
    sqlite3_reset(stmt_all_volumes_);
    while (sqlite3_step(stmt_all_volumes_) == SQLITE_ROW)
        result.push_back(row_to_volume(stmt_all_volumes_));
    return result;
}

bool PersistentCatalog::delete_volume(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_delete_volume_);
    sqlite3_bind_text(stmt_delete_volume_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt_delete_volume_) == SQLITE_DONE;
}

// ── CRUD ────────────────────────────────────────────────────

bool PersistentCatalog::add_file(const FileRecord& file) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_add_file_);
    sqlite3_bind_text(stmt_add_file_, 1, file.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_add_file_, 2, file.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_add_file_, 3, file.extension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_add_file_, 4, static_cast<int>(file.file_type));
    sqlite3_bind_int(stmt_add_file_, 5, static_cast<int>(file.current_tier));
    sqlite3_bind_int(stmt_add_file_, 6, static_cast<int>(file.target_tier));
    sqlite3_bind_int64(stmt_add_file_, 7, file.size_bytes);
    sqlite3_bind_int(stmt_add_file_, 8, file.access_count);
    sqlite3_bind_int(stmt_add_file_, 9, file.write_count);
    sqlite3_bind_int64(stmt_add_file_, 10, static_cast<int64_t>(file.created_at));
    sqlite3_bind_int64(stmt_add_file_, 11, static_cast<int64_t>(file.last_accessed));
    sqlite3_bind_int64(stmt_add_file_, 12, static_cast<int64_t>(file.last_modified));
    sqlite3_bind_int(stmt_add_file_, 13, file.migrate_count);
    sqlite3_bind_int(stmt_add_file_, 14, file.is_pinned ? 1 : 0);
    sqlite3_bind_int(stmt_add_file_, 15, file.is_critical ? 1 : 0);
    sqlite3_bind_double(stmt_add_file_, 16, file.score);
    sqlite3_bind_text(stmt_add_file_, 17, file.s3_bucket.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_add_file_, 18, file.s3_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_add_file_, 19, file.content_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_add_file_, 20, file.etag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_add_file_, 21, file.owner_id.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt_add_file_) == SQLITE_DONE;
}

bool PersistentCatalog::update_file(const FileRecord& file) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_update_file_);
    sqlite3_bind_text(stmt_update_file_, 1, file.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_file_, 2, file.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_file_, 3, file.extension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_update_file_, 4, static_cast<int>(file.file_type));
    sqlite3_bind_int(stmt_update_file_, 5, static_cast<int>(file.current_tier));
    sqlite3_bind_int(stmt_update_file_, 6, static_cast<int>(file.target_tier));
    sqlite3_bind_int64(stmt_update_file_, 7, file.size_bytes);
    sqlite3_bind_int(stmt_update_file_, 8, file.access_count);
    sqlite3_bind_int(stmt_update_file_, 9, file.write_count);
    sqlite3_bind_int64(stmt_update_file_, 10, static_cast<int64_t>(file.created_at));
    sqlite3_bind_int64(stmt_update_file_, 11, static_cast<int64_t>(file.last_accessed));
    sqlite3_bind_int64(stmt_update_file_, 12, static_cast<int64_t>(file.last_modified));
    sqlite3_bind_int(stmt_update_file_, 13, file.migrate_count);
    sqlite3_bind_int(stmt_update_file_, 14, file.is_pinned ? 1 : 0);
    sqlite3_bind_int(stmt_update_file_, 15, file.is_critical ? 1 : 0);
    sqlite3_bind_double(stmt_update_file_, 16, file.score);
    sqlite3_bind_text(stmt_update_file_, 17, file.s3_bucket.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_file_, 18, file.s3_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_file_, 19, file.content_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_file_, 20, file.etag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_file_, 21, file.owner_id.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt_update_file_) == SQLITE_DONE;
}

bool PersistentCatalog::upsert_file(const FileRecord& file) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_upsert_file_);
    sqlite3_bind_text(stmt_upsert_file_, 1, file.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_upsert_file_, 2, file.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_upsert_file_, 3, file.extension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_upsert_file_, 4, static_cast<int>(file.file_type));
    sqlite3_bind_int(stmt_upsert_file_, 5, static_cast<int>(file.current_tier));
    sqlite3_bind_int(stmt_upsert_file_, 6, static_cast<int>(file.target_tier));
    sqlite3_bind_int64(stmt_upsert_file_, 7, file.size_bytes);
    sqlite3_bind_int(stmt_upsert_file_, 8, file.access_count);
    sqlite3_bind_int(stmt_upsert_file_, 9, file.write_count);
    sqlite3_bind_int64(stmt_upsert_file_, 10, static_cast<int64_t>(file.created_at));
    sqlite3_bind_int64(stmt_upsert_file_, 11, static_cast<int64_t>(file.last_accessed));
    sqlite3_bind_int64(stmt_upsert_file_, 12, static_cast<int64_t>(file.last_modified));
    sqlite3_bind_int(stmt_upsert_file_, 13, file.migrate_count);
    sqlite3_bind_int(stmt_upsert_file_, 14, file.is_pinned ? 1 : 0);
    sqlite3_bind_int(stmt_upsert_file_, 15, file.is_critical ? 1 : 0);
    sqlite3_bind_double(stmt_upsert_file_, 16, file.score);
    sqlite3_bind_text(stmt_upsert_file_, 17, file.s3_bucket.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_upsert_file_, 18, file.s3_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_upsert_file_, 19, file.content_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_upsert_file_, 20, file.etag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_upsert_file_, 21, file.owner_id.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt_upsert_file_) == SQLITE_DONE;
}

std::optional<FileRecord> PersistentCatalog::get_file(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_get_file_);
    sqlite3_bind_text(stmt_get_file_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt_get_file_);
    if (rc == SQLITE_ROW)
        return row_to_file_record(stmt_get_file_);
    return std::nullopt;
}

std::optional<FileRecord> PersistentCatalog::get_file_by_path(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_get_file_by_path_);
    sqlite3_bind_text(stmt_get_file_by_path_, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt_get_file_by_path_);
    if (rc == SQLITE_ROW)
        return row_to_file_record(stmt_get_file_by_path_);
    return std::nullopt;
}

bool PersistentCatalog::delete_file(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_delete_file_);
    sqlite3_bind_text(stmt_delete_file_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt_delete_file_) == SQLITE_DONE;
}

// ── Queries ──────────────────────────────────────────────────

std::vector<FileRecord> PersistentCatalog::all_files() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<FileRecord> result;
    const char* sql = "SELECT id,path,extension,file_type,current_tier,target_tier,"
                      "size_bytes,access_count,write_count,created_at,last_accessed,"
                      "last_modified,migrate_count,is_pinned,is_critical,score,owner_id,"
                      "s3_bucket,s3_key,content_type,etag FROM files";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(row_to_file_record(stmt));
    sqlite3_finalize(stmt);
    return result;
}

std::vector<FileRecord> PersistentCatalog::files_by_owner(const std::string& owner_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<FileRecord> result;
    const char* sql = "SELECT id,path,extension,file_type,current_tier,target_tier,"
                      "size_bytes,access_count,write_count,created_at,last_accessed,"
                      "last_modified,migrate_count,is_pinned,is_critical,score,owner_id,"
                      "owner_id,s3_bucket,s3_key,content_type,etag "
                      "FROM files WHERE owner_id=?1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;
    sqlite3_bind_text(stmt, 1, owner_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(row_to_file_record(stmt));
    sqlite3_finalize(stmt);
    return result;
}

std::vector<FileRecord> PersistentCatalog::files_by_tier(Tier t) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<FileRecord> result;
    const char* sql = "SELECT id,path,extension,file_type,current_tier,target_tier,"
                      "size_bytes,access_count,write_count,created_at,last_accessed,"
                      "last_modified,migrate_count,is_pinned,is_critical,score,owner_id,"
                      "s3_bucket,s3_key,content_type,etag "
                      "FROM files WHERE current_tier=?1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;
    sqlite3_bind_int(stmt, 1, static_cast<int>(t));
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(row_to_file_record(stmt));
    sqlite3_finalize(stmt);
    return result;
}

std::vector<FileRecord> PersistentCatalog::files_needing_migration() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<FileRecord> result;
    const char* sql = "SELECT id,path,extension,file_type,current_tier,target_tier,"
                      "size_bytes,access_count,write_count,created_at,last_accessed,"
                      "last_modified,migrate_count,is_pinned,is_critical,score,owner_id,"
                      "s3_bucket,s3_key,content_type,etag "
                      "FROM files WHERE current_tier != target_tier AND is_pinned = 0";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        result.push_back(row_to_file_record(stmt));
    sqlite3_finalize(stmt);
    return result;
}

int PersistentCatalog::file_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "SELECT COUNT(*) FROM files";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int count = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return count;
}

int PersistentCatalog::file_count_by_tier(Tier t) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "SELECT COUNT(*) FROM files WHERE current_tier=?1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, static_cast<int>(t));
    int count = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return count;
}

// ── Stats ────────────────────────────────────────────────────

TierStats PersistentCatalog::stats_for_tier(Tier t) const {
    TierStats s;
    s.tier = t;
    const char* sql = "SELECT COUNT(*), COALESCE(SUM(size_bytes),0), "
                      "COALESCE(SUM(size_bytes/1073741824.0 * ?2 * 30), 0), "
                      "COALESCE(SUM(access_count), 0) "
                      "FROM files WHERE current_tier=?1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return s;
    sqlite3_bind_int(stmt, 1, static_cast<int>(t));
    sqlite3_bind_double(stmt, 2, tier_cost_per_gb(t));
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        s.file_count      = sqlite3_column_int(stmt, 0);
        s.total_bytes     = sqlite3_column_int64(stmt, 1);
        s.monthly_cost_usd= sqlite3_column_double(stmt, 2);
        s.total_accesses  = sqlite3_column_int(stmt, 3);
    }
    sqlite3_finalize(stmt);
    return s;
}

std::array<TierStats, 4> PersistentCatalog::all_tier_stats() const {
    std::array<TierStats, 4> stats;
    const char* sql = "SELECT current_tier, COUNT(*), COALESCE(SUM(size_bytes),0), "
                      "COALESCE(SUM(access_count),0) "
                      "FROM files GROUP BY current_tier";
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return stats;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int idx = sqlite3_column_int(stmt, 0);
        if (idx >= 0 && idx < 4) {
            stats[idx].tier      = static_cast<Tier>(idx);
            stats[idx].file_count= sqlite3_column_int(stmt, 1);
            stats[idx].total_bytes= sqlite3_column_int64(stmt, 2);
            stats[idx].total_accesses = sqlite3_column_int(stmt, 3);
            stats[idx].monthly_cost_usd = stats[idx].total_gb() *
                                          tier_cost_per_gb(stats[idx].tier) * 30.0;
        }
    }
    sqlite3_finalize(stmt);
    return stats;
}

int64_t PersistentCatalog::total_bytes() const {
    const char* sql = "SELECT COALESCE(SUM(size_bytes), 0) FROM files";
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int64_t bytes = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        bytes = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return bytes;
}

double PersistentCatalog::total_monthly_cost() const {
    const char* sql = "SELECT COALESCE(SUM(size_bytes/1073741824.0 * "
                      "CASE current_tier "
                      "  WHEN 0 THEN 0.20 WHEN 1 THEN 0.05 "
                      "  WHEN 2 THEN 0.01 WHEN 3 THEN 0.002 "
                      "END * 30), 0) FROM files";
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0.0;
    double cost = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        cost = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return cost;
}

// ── Access Tracking ──────────────────────────────────────────

bool PersistentCatalog::record_access(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_record_access_);
    sqlite3_bind_int64(stmt_record_access_, 1, static_cast<int64_t>(std::time(nullptr)));
    sqlite3_bind_text(stmt_record_access_, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt_record_access_) == SQLITE_DONE;
}

bool PersistentCatalog::record_write(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "UPDATE files SET write_count=write_count+1, "
                      "last_modified=?1 WHERE id=?2";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(std::time(nullptr)));
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// ── Batch Operations ─────────────────────────────────────────

bool PersistentCatalog::bulk_update_tier(const std::vector<std::string>& ids, Tier new_tier) {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "UPDATE files SET current_tier=?1, target_tier=?1 WHERE id=?2";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool all_ok = true;
    for (const auto& id : ids) {
        sqlite3_reset(stmt);
        sqlite3_bind_int(stmt, 1, static_cast<int>(new_tier));
        sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) all_ok = false;
    }
    sqlite3_finalize(stmt);
    return all_ok;
}

bool PersistentCatalog::bulk_set_pinned(const std::vector<std::string>& ids, bool pinned) {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "UPDATE files SET is_pinned=?1 WHERE id=?2";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool all_ok = true;
    for (const auto& id : ids) {
        sqlite3_reset(stmt);
        sqlite3_bind_int(stmt, 1, pinned ? 1 : 0);
        sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) all_ok = false;
    }
    sqlite3_finalize(stmt);
    return all_ok;
}

// ── Migration History ────────────────────────────────────────

bool PersistentCatalog::log_migration(const MigrationEvent& event) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_reset(stmt_log_migration_);
    sqlite3_bind_text(stmt_log_migration_, 1, event.file_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_log_migration_, 2, event.file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_log_migration_, 3, static_cast<int>(event.from_tier));
    sqlite3_bind_int(stmt_log_migration_, 4, static_cast<int>(event.to_tier));
    sqlite3_bind_int64(stmt_log_migration_, 5, event.size_bytes);
    sqlite3_bind_text(stmt_log_migration_, 6, event.reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_log_migration_, 7, static_cast<int64_t>(event.timestamp));
    sqlite3_bind_int(stmt_log_migration_, 8, event.success ? 1 : 0);
    sqlite3_bind_double(stmt_log_migration_, 9, event.duration_ms);
    return sqlite3_step(stmt_log_migration_) == SQLITE_DONE;
}

std::vector<MigrationEvent> PersistentCatalog::recent_migrations(int n) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<MigrationEvent> result;
    const char* sql = "SELECT file_id,file_path,from_tier,to_tier,size_bytes,"
                      "COALESCE(reason,''),timestamp,success,duration_ms "
                      "FROM migration_history ORDER BY id DESC LIMIT ?1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_int(stmt, 1, n);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MigrationEvent e;
        e.file_id    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        e.file_path  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        e.from_tier  = static_cast<Tier>(sqlite3_column_int(stmt, 2));
        e.to_tier    = static_cast<Tier>(sqlite3_column_int(stmt, 3));
        e.size_bytes = sqlite3_column_int64(stmt, 4);
        e.reason     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        e.timestamp  = sqlite3_column_int64(stmt, 6);
        e.success    = sqlite3_column_int(stmt, 7) != 0;
        e.duration_ms= sqlite3_column_double(stmt, 8);
        result.push_back(e);
    }
    sqlite3_finalize(stmt);
    return result;
}

int64_t PersistentCatalog::total_bytes_migrated() const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "SELECT COALESCE(SUM(size_bytes), 0) FROM migration_history WHERE success=1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int64_t bytes = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return bytes;
}

int PersistentCatalog::total_migrations() const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "SELECT COUNT(*) FROM migration_history WHERE success=1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int count = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return count;
}

bool PersistentCatalog::clear_history() {
    return exec_sql("DELETE FROM migration_history");
}

// ── Maintenance ──────────────────────────────────────────────

bool PersistentCatalog::vacuum() {
    return exec_sql("VACUUM");
}

bool PersistentCatalog::checkpoint() {
    return exec_sql("PRAGMA wal_checkpoint(TRUNCATE)");
}
