#pragma once

#include "common/types.h"
#include "common/logger.h"
#include <sqlite3.h>
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <thread>
#include <chrono>
#include <cstring>
#include <sstream>
#include <memory>

namespace ecpb {

// ─── Global DB Mutex ─────────────────────────────────────────────────
// All modules share this recursive mutex when performing DB operations
// to prevent SQLITE_BUSY errors from concurrent access within the same
// process. Recursive so nested DB calls (e.g. store_file_manifest
// calling chunk_exists internally) won't deadlock.
inline std::recursive_mutex& db_global_mutex() {
    static std::recursive_mutex mtx;
    return mtx;
}

// RAII lock guard for database operations
class DBLock {
public:
    DBLock() : lock_(db_global_mutex()) {}
private:
    std::lock_guard<std::recursive_mutex> lock_;
};

// ─── RAII Transaction wrapper ────────────────────────────────────────
// Uses BEGIN IMMEDIATE to acquire a write lock immediately, preventing
// SQLITE_BUSY when multiple modules try to write concurrently.
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db), committed_(false) {
        // BEGIN IMMEDIATE acquires a RESERVED lock right away, avoiding
        // SQLITE_BUSY race on the COMMIT step.
        char* errmsg = nullptr;
        int rc = sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            LOG_ERR("Transaction BEGIN failed: %s", errmsg ? errmsg : "unknown");
            if (errmsg) sqlite3_free(errmsg);
            active_ = false;
        } else {
            active_ = true;
        }
    }

    ~Transaction() {
        if (active_ && !committed_) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool commit() {
        if (!active_ || committed_) return false;
        char* errmsg = nullptr;
        int rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            LOG_ERR("Transaction COMMIT failed: %s", errmsg ? errmsg : "unknown");
            if (errmsg) sqlite3_free(errmsg);
            return false;
        }
        committed_ = true;
        return true;
    }

    bool is_active() const { return active_ && !committed_; }

private:
    sqlite3* db_;
    bool committed_;
    bool active_;
};

// ─── RAII Statement wrapper ──────────────────────────────────────────
class Statement {
public:
    Statement() : stmt_(nullptr) {}
    ~Statement() { finalize(); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& o) noexcept : stmt_(o.stmt_) { o.stmt_ = nullptr; }

    bool prepare(sqlite3* db, const char* sql) {
        finalize();
        int rc = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
        return rc == SQLITE_OK;
    }

    void finalize() {
        if (stmt_) { sqlite3_finalize(stmt_); stmt_ = nullptr; }
    }

    bool bind_text(int idx, const std::string& val) {
        return sqlite3_bind_text(stmt_, idx, val.c_str(), static_cast<int>(val.size()), SQLITE_TRANSIENT) == SQLITE_OK;
    }
    bool bind_int(int idx, int val) {
        return sqlite3_bind_int(stmt_, idx, val) == SQLITE_OK;
    }
    bool bind_int64(int idx, int64_t val) {
        return sqlite3_bind_int64(stmt_, idx, val) == SQLITE_OK;
    }
    bool bind_blob(int idx, const void* data, int len) {
        return sqlite3_bind_blob(stmt_, idx, data, len, SQLITE_TRANSIENT) == SQLITE_OK;
    }

    // Step with automatic SQLITE_BUSY retry
    int step_retry(int max_retries = SQLITE_MAX_RETRIES) {
        for (int attempt = 0; attempt < max_retries; ++attempt) {
            int rc = sqlite3_step(stmt_);
            if (rc != SQLITE_BUSY) return rc;
            LOG_WARN("SQLite busy, retry %d/%d", attempt + 1, max_retries);
            std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
        }
        return SQLITE_BUSY;
    }

    int step() { return step_retry(); }

    bool reset() { return sqlite3_reset(stmt_) == SQLITE_OK; }

    const char* column_text(int col) {
        auto ptr = sqlite3_column_text(stmt_, col);
        return ptr ? reinterpret_cast<const char*>(ptr) : "";
    }
    int column_int(int col) { return sqlite3_column_int(stmt_, col); }
    int64_t column_int64(int col) { return sqlite3_column_int64(stmt_, col); }
    const void* column_blob(int col) { return sqlite3_column_blob(stmt_, col); }
    int column_bytes(int col) { return sqlite3_column_bytes(stmt_, col); }

    sqlite3_stmt* raw() { return stmt_; }

private:
    sqlite3_stmt* stmt_;
};

// ─── Database ────────────────────────────────────────────────────────
class Database {
public:
    Database() : db_(nullptr) {}
    ~Database() { close(); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const std::string& path) {
        DBLock lock;
        if (db_) close();
        db_path_ = path;
        int rc = sqlite3_open(path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            LOG_ERR("Database: failed to open %s: %s", path.c_str(), sqlite3_errmsg(db_));
            db_ = nullptr;
            return false;
        }
        // Set busy timeout at connection level (5 seconds)
        sqlite3_busy_timeout(db_, SQLITE_BUSY_TIMEOUT_MS);
        // Enable WAL mode for better concurrency across processes
        exec_simple("PRAGMA journal_mode=WAL");
        // NORMAL sync balances durability and performance
        exec_simple("PRAGMA synchronous=NORMAL");
        exec_simple("PRAGMA foreign_keys=ON");
        // Auto-checkpoint every 1000 pages to prevent WAL from growing too large
        exec_simple("PRAGMA wal_autocheckpoint=1000");
        // Increase cache for better read performance during concurrent access
        exec_simple("PRAGMA cache_size=-8000");  // 8MB cache
        return create_tables();
    }

    void close() {
        DBLock lock;
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
            LOG_INFO("Database closed");
        }
    }

    bool is_open() const { return db_ != nullptr; }

    // ─── Job Operations ──────────────────────────────────────────
    int create_job(const BackupJob& job) {
        DBLock lock;
        Statement stmt;
        if (!stmt.prepare(db_,
            "INSERT INTO jobs (source_path, backup_name, status, priority, compression, "
            "encrypt, incremental, parent_job_id, created_at) "
            "VALUES (?,?,?,?,?,?,?,?,?)")) {
            LOG_ERR("DB: prepare create_job failed: %s", sqlite3_errmsg(db_));
            return -1;
        }
        stmt.bind_text(1, job.source_path);
        stmt.bind_text(2, job.backup_name);
        stmt.bind_int(3, static_cast<int>(job.status));
        stmt.bind_int(4, static_cast<int>(job.priority));
        stmt.bind_int(5, static_cast<int>(job.compression));
        stmt.bind_int(6, job.encrypt ? 1 : 0);
        stmt.bind_int(7, job.incremental ? 1 : 0);
        stmt.bind_int(8, job.parent_job_id);
        stmt.bind_int64(9, static_cast<int64_t>(now_epoch_ms()));

        int rc = stmt.step();
        if (rc != SQLITE_DONE) {
            LOG_ERR("DB: create_job failed: %s", sqlite3_errmsg(db_));
            return -1;
        }
        return static_cast<int>(sqlite3_last_insert_rowid(db_));
    }

    bool update_job_status(int job_id, JobStatus status, const std::string& error = "") {
        DBLock lock;
        std::string sql;
        if (status == JobStatus::RUNNING) {
            sql = "UPDATE jobs SET status=?, started_at=? WHERE job_id=?";
        } else if (status == JobStatus::COMPLETED || status == JobStatus::FAILED) {
            sql = "UPDATE jobs SET status=?, completed_at=?, error_message=? WHERE job_id=?";
        } else {
            sql = "UPDATE jobs SET status=? WHERE job_id=?";
        }

        Statement stmt;
        if (!stmt.prepare(db_, sql.c_str())) return false;

        stmt.bind_int(1, static_cast<int>(status));
        if (status == JobStatus::RUNNING) {
            stmt.bind_int64(2, static_cast<int64_t>(now_epoch_ms()));
            stmt.bind_int(3, job_id);
        } else if (status == JobStatus::COMPLETED || status == JobStatus::FAILED) {
            stmt.bind_int64(2, static_cast<int64_t>(now_epoch_ms()));
            stmt.bind_text(3, error);
            stmt.bind_int(4, job_id);
        } else {
            stmt.bind_int(2, job_id);
        }
        return stmt.step() == SQLITE_DONE;
    }

    bool update_job_stats(int job_id, uint64_t total_bytes, uint64_t processed_bytes,
                          uint64_t stored_bytes, uint64_t dedup_savings, int file_count) {
        DBLock lock;
        Statement stmt;
        if (!stmt.prepare(db_,
            "UPDATE jobs SET total_bytes=?, processed_bytes=?, stored_bytes=?, "
            "dedup_savings=?, file_count=? WHERE job_id=?")) return false;
        stmt.bind_int64(1, static_cast<int64_t>(total_bytes));
        stmt.bind_int64(2, static_cast<int64_t>(processed_bytes));
        stmt.bind_int64(3, static_cast<int64_t>(stored_bytes));
        stmt.bind_int64(4, static_cast<int64_t>(dedup_savings));
        stmt.bind_int(5, file_count);
        stmt.bind_int(6, job_id);
        return stmt.step() == SQLITE_DONE;
    }

    std::optional<BackupJob> get_job(int job_id) {
        DBLock lock;
        Statement stmt;
        if (!stmt.prepare(db_, "SELECT * FROM jobs WHERE job_id=?")) return std::nullopt;
        stmt.bind_int(1, job_id);
        if (stmt.step() != SQLITE_ROW) return std::nullopt;
        return row_to_job(stmt);
    }

    std::vector<BackupJob> get_all_jobs() {
        DBLock lock;
        std::vector<BackupJob> jobs;
        Statement stmt;
        if (!stmt.prepare(db_, "SELECT * FROM jobs ORDER BY created_at DESC")) return jobs;
        while (stmt.step() == SQLITE_ROW) {
            jobs.push_back(row_to_job(stmt));
        }
        return jobs;
    }

    std::vector<BackupJob> get_jobs_by_status(JobStatus status) {
        DBLock lock;
        std::vector<BackupJob> jobs;
        Statement stmt;
        if (!stmt.prepare(db_, "SELECT * FROM jobs WHERE status=? ORDER BY priority DESC, created_at ASC")) return jobs;
        stmt.bind_int(1, static_cast<int>(status));
        while (stmt.step() == SQLITE_ROW) {
            jobs.push_back(row_to_job(stmt));
        }
        return jobs;
    }

    // ─── Chunk Operations ────────────────────────────────────────
    bool store_chunk(const std::string& hash_hex, const std::string& storage_path,
                     uint32_t original_size, uint32_t stored_size,
                     int compression, bool encrypted, int ref_count = 1) {
        DBLock lock;
        Transaction txn(db_);
        if (!txn.is_active()) return false;

        Statement stmt;
        if (!stmt.prepare(db_,
            "INSERT OR IGNORE INTO chunks (hash, storage_path, original_size, "
            "stored_size, compression, encrypted, ref_count) "
            "VALUES (?,?,?,?,?,?,?)")) return false;
        stmt.bind_text(1, hash_hex);
        stmt.bind_text(2, storage_path);
        stmt.bind_int(3, static_cast<int>(original_size));
        stmt.bind_int(4, static_cast<int>(stored_size));
        stmt.bind_int(5, compression);
        stmt.bind_int(6, encrypted ? 1 : 0);
        stmt.bind_int(7, ref_count);
        int rc = stmt.step();
        if (rc == SQLITE_DONE) {
            // If already existed (IGNORE), increment ref_count
            if (sqlite3_changes(db_) == 0) {
                if (!increment_chunk_ref(hash_hex)) return false;
            }
            return txn.commit();
        }
        return false;
    }

    bool chunk_exists(const std::string& hash_hex) {
        DBLock lock;
        Statement stmt;
        if (!stmt.prepare(db_, "SELECT 1 FROM chunks WHERE hash=?")) return false;
        stmt.bind_text(1, hash_hex);
        return stmt.step() == SQLITE_ROW;
    }

    std::string get_chunk_path(const std::string& hash_hex) {
        DBLock lock;
        Statement stmt;
        if (!stmt.prepare(db_, "SELECT storage_path FROM chunks WHERE hash=?")) return "";
        stmt.bind_text(1, hash_hex);
        if (stmt.step() != SQLITE_ROW) return "";
        return stmt.column_text(0);
    }

    struct ChunkMeta {
        std::string hash;
        std::string storage_path;
        uint32_t original_size;
        uint32_t stored_size;
        int compression;
        bool encrypted;
        int ref_count;
    };

    std::optional<ChunkMeta> get_chunk_meta(const std::string& hash_hex) {
        DBLock lock;
        Statement stmt;
        if (!stmt.prepare(db_, "SELECT hash, storage_path, original_size, stored_size, "
                                "compression, encrypted, ref_count FROM chunks WHERE hash=?")) return std::nullopt;
        stmt.bind_text(1, hash_hex);
        if (stmt.step() != SQLITE_ROW) return std::nullopt;
        ChunkMeta cm;
        cm.hash = stmt.column_text(0);
        cm.storage_path = stmt.column_text(1);
        cm.original_size = static_cast<uint32_t>(stmt.column_int(2));
        cm.stored_size = static_cast<uint32_t>(stmt.column_int(3));
        cm.compression = stmt.column_int(4);
        cm.encrypted = stmt.column_int(5) != 0;
        cm.ref_count = stmt.column_int(6);
        return cm;
    }

    // ─── File Manifest Operations ────────────────────────────────
    bool store_file_manifest(int job_id, const FileManifest& manifest) {
        DBLock lock;
        Transaction txn(db_);
        if (!txn.is_active()) return false;

        Statement stmt;
        if (!stmt.prepare(db_,
            "INSERT INTO file_manifests (job_id, file_path, file_name, file_size, "
            "modified_time, file_hash) VALUES (?,?,?,?,?,?)")) return false;
        stmt.bind_int(1, job_id);
        stmt.bind_text(2, manifest.file_path);
        stmt.bind_text(3, manifest.file_name);
        stmt.bind_int64(4, static_cast<int64_t>(manifest.file_size));
        stmt.bind_int64(5, static_cast<int64_t>(manifest.modified_time));
        stmt.bind_text(6, manifest.file_hash.str());
        if (stmt.step() != SQLITE_DONE) return false;
        int manifest_id = static_cast<int>(sqlite3_last_insert_rowid(db_));

        // Store chunk references in same transaction
        Statement chunk_stmt;
        if (!chunk_stmt.prepare(db_,
            "INSERT INTO file_chunks (manifest_id, chunk_hash, chunk_index, offset, size, deduplicated) "
            "VALUES (?,?,?,?,?,?)")) return false;
        for (auto& chunk : manifest.chunks) {
            chunk_stmt.bind_int(1, manifest_id);
            chunk_stmt.bind_text(2, chunk.hash.str());
            chunk_stmt.bind_int(3, static_cast<int>(chunk.chunk_index));
            chunk_stmt.bind_int64(4, static_cast<int64_t>(chunk.offset));
            chunk_stmt.bind_int(5, static_cast<int>(chunk.size));
            chunk_stmt.bind_int(6, chunk.deduplicated ? 1 : 0);
            if (chunk_stmt.step() != SQLITE_DONE) return false;
            chunk_stmt.reset();
        }
        return txn.commit();
    }

    std::vector<FileManifest> get_file_manifests(int job_id) {
        DBLock lock;
        std::vector<FileManifest> manifests;
        Statement stmt;
        if (!stmt.prepare(db_,
            "SELECT manifest_id, file_path, file_name, file_size, modified_time, file_hash "
            "FROM file_manifests WHERE job_id=?")) return manifests;
        stmt.bind_int(1, job_id);

        while (stmt.step() == SQLITE_ROW) {
            FileManifest m;
            int manifest_id = stmt.column_int(0);
            m.file_path = stmt.column_text(1);
            m.file_name = stmt.column_text(2);
            m.file_size = static_cast<uint64_t>(stmt.column_int64(3));
            m.modified_time = static_cast<uint64_t>(stmt.column_int64(4));
            std::string hash_str = stmt.column_text(5);
            std::strncpy(m.file_hash.data, hash_str.c_str(), SHA256_HEX_LEN);

            // Load chunks for this manifest
            Statement cstmt;
            if (cstmt.prepare(db_,
                "SELECT chunk_hash, chunk_index, offset, size, deduplicated "
                "FROM file_chunks WHERE manifest_id=? ORDER BY chunk_index")) {
                cstmt.bind_int(1, manifest_id);
                while (cstmt.step() == SQLITE_ROW) {
                    ChunkInfo ci;
                    std::string ch = cstmt.column_text(0);
                    std::strncpy(ci.hash.data, ch.c_str(), SHA256_HEX_LEN);
                    ci.chunk_index = static_cast<uint32_t>(cstmt.column_int(1));
                    ci.offset = static_cast<uint64_t>(cstmt.column_int64(2));
                    ci.size = static_cast<uint32_t>(cstmt.column_int(3));
                    ci.deduplicated = cstmt.column_int(4) != 0;
                    m.chunks.push_back(ci);
                }
            }
            manifests.push_back(std::move(m));
        }
        return manifests;
    }

    // ─── Encryption Key Storage ──────────────────────────────────
    bool store_encryption_key(int job_id, const std::string& key_hex) {
        DBLock lock;
        Statement stmt;
        if (!stmt.prepare(db_,
            "INSERT OR REPLACE INTO encryption_keys (job_id, key_hex) VALUES (?,?)")) return false;
        stmt.bind_int(1, job_id);
        stmt.bind_text(2, key_hex);
        return stmt.step() == SQLITE_DONE;
    }

    std::string get_encryption_key(int job_id) {
        DBLock lock;
        Statement stmt;
        if (!stmt.prepare(db_, "SELECT key_hex FROM encryption_keys WHERE job_id=?")) return "";
        stmt.bind_int(1, job_id);
        if (stmt.step() != SQLITE_ROW) return "";
        return stmt.column_text(0);
    }

    // ─── Dependency Operations ───────────────────────────────────
    bool add_dependency(int job_id, int depends_on) {
        DBLock lock;
        Statement stmt;
        if (!stmt.prepare(db_,
            "INSERT OR IGNORE INTO job_dependencies (job_id, depends_on) VALUES (?,?)")) return false;
        stmt.bind_int(1, job_id);
        stmt.bind_int(2, depends_on);
        return stmt.step() == SQLITE_DONE;
    }

    std::vector<int> get_dependencies(int job_id) {
        DBLock lock;
        std::vector<int> deps;
        Statement stmt;
        if (!stmt.prepare(db_, "SELECT depends_on FROM job_dependencies WHERE job_id=?")) return deps;
        stmt.bind_int(1, job_id);
        while (stmt.step() == SQLITE_ROW) {
            deps.push_back(stmt.column_int(0));
        }
        return deps;
    }

    // ─── Messaging ───────────────────────────────────────────────
    int create_channel(const std::string& name) {
        DBLock lock;
        Statement stmt;
        if (!stmt.prepare(db_,
            "INSERT OR IGNORE INTO channels (name, created_at) VALUES (?,?)")) return -1;
        stmt.bind_text(1, name);
        stmt.bind_int64(2, static_cast<int64_t>(now_epoch_ms()));
        if (stmt.step() != SQLITE_DONE) return -1;
        if (sqlite3_changes(db_) == 0) {
            // Already exists - look it up
            Statement find;
            if (!find.prepare(db_, "SELECT channel_id FROM channels WHERE name=?")) return -1;
            find.bind_text(1, name);
            if (find.step() == SQLITE_ROW) return find.column_int(0);
            return -1;
        }
        return static_cast<int>(sqlite3_last_insert_rowid(db_));
    }

    bool send_message(const std::string& channel, const std::string& sender,
                      const std::string& content, const std::string& msg_type = "text") {
        DBLock lock;
        Statement stmt;
        if (!stmt.prepare(db_,
            "INSERT INTO messages (channel_name, sender, content, msg_type, created_at) "
            "VALUES (?,?,?,?,?)")) return false;
        stmt.bind_text(1, channel);
        stmt.bind_text(2, sender);
        stmt.bind_text(3, content);
        stmt.bind_text(4, msg_type);
        stmt.bind_int64(5, static_cast<int64_t>(now_epoch_ms()));
        return stmt.step() == SQLITE_DONE;
    }

    struct Message {
        int id;
        std::string channel;
        std::string sender;
        std::string content;
        std::string msg_type;
        uint64_t created_at;
    };

    std::vector<Message> get_messages(const std::string& channel, int limit = 50) {
        DBLock lock;
        std::vector<Message> msgs;
        Statement stmt;
        if (!stmt.prepare(db_,
            "SELECT msg_id, channel_name, sender, content, msg_type, created_at "
            "FROM messages WHERE channel_name=? ORDER BY created_at DESC LIMIT ?")) return msgs;
        stmt.bind_text(1, channel);
        stmt.bind_int(2, limit);
        while (stmt.step() == SQLITE_ROW) {
            Message m;
            m.id = stmt.column_int(0);
            m.channel = stmt.column_text(1);
            m.sender = stmt.column_text(2);
            m.content = stmt.column_text(3);
            m.msg_type = stmt.column_text(4);
            m.created_at = static_cast<uint64_t>(stmt.column_int64(5));
            msgs.push_back(std::move(m));
        }
        // Reverse so oldest first
        std::reverse(msgs.begin(), msgs.end());
        return msgs;
    }

    // ─── Statistics ──────────────────────────────────────────────
    struct DBStats {
        int total_jobs;
        int completed_jobs;
        int failed_jobs;
        int total_chunks;
        uint64_t total_stored_bytes;
        uint64_t total_dedup_savings;
        int total_files;
    };

    DBStats get_stats() {
        DBLock lock;
        DBStats stats{};
        Statement stmt;

        if (stmt.prepare(db_, "SELECT COUNT(*) FROM jobs")) {
            if (stmt.step() == SQLITE_ROW) stats.total_jobs = stmt.column_int(0);
        }
        if (stmt.prepare(db_, "SELECT COUNT(*) FROM jobs WHERE status=?")) {
            stmt.bind_int(1, static_cast<int>(JobStatus::COMPLETED));
            if (stmt.step() == SQLITE_ROW) stats.completed_jobs = stmt.column_int(0);
        }
        if (stmt.prepare(db_, "SELECT COUNT(*) FROM jobs WHERE status=?")) {
            stmt.bind_int(1, static_cast<int>(JobStatus::FAILED));
            if (stmt.step() == SQLITE_ROW) stats.failed_jobs = stmt.column_int(0);
        }
        if (stmt.prepare(db_, "SELECT COUNT(*), COALESCE(SUM(stored_size),0) FROM chunks")) {
            if (stmt.step() == SQLITE_ROW) {
                stats.total_chunks = stmt.column_int(0);
                stats.total_stored_bytes = static_cast<uint64_t>(stmt.column_int64(1));
            }
        }
        if (stmt.prepare(db_, "SELECT COALESCE(SUM(dedup_savings),0) FROM jobs")) {
            if (stmt.step() == SQLITE_ROW) stats.total_dedup_savings = static_cast<uint64_t>(stmt.column_int64(0));
        }
        if (stmt.prepare(db_, "SELECT COUNT(*) FROM file_manifests")) {
            if (stmt.step() == SQLITE_ROW) stats.total_files = stmt.column_int(0);
        }
        return stats;
    }

    sqlite3* raw() { return db_; }

private:
    sqlite3* db_;
    std::string db_path_;

    bool exec_simple(const char* sql) {
        char* errmsg = nullptr;
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            LOG_ERR("SQL exec failed: %s (sql: %s)", errmsg ? errmsg : "unknown", sql);
            if (errmsg) sqlite3_free(errmsg);
            return false;
        }
        return true;
    }

    bool create_tables() {
        exec_simple("BEGIN IMMEDIATE");
        const char* schemas[] = {
            "CREATE TABLE IF NOT EXISTS jobs ("
            "  job_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  source_path TEXT NOT NULL,"
            "  backup_name TEXT NOT NULL,"
            "  status INTEGER DEFAULT 0,"
            "  priority INTEGER DEFAULT 1,"
            "  compression INTEGER DEFAULT 1,"
            "  encrypt INTEGER DEFAULT 1,"
            "  incremental INTEGER DEFAULT 0,"
            "  parent_job_id INTEGER DEFAULT -1,"
            "  created_at INTEGER,"
            "  started_at INTEGER,"
            "  completed_at INTEGER,"
            "  total_bytes INTEGER DEFAULT 0,"
            "  processed_bytes INTEGER DEFAULT 0,"
            "  stored_bytes INTEGER DEFAULT 0,"
            "  dedup_savings INTEGER DEFAULT 0,"
            "  file_count INTEGER DEFAULT 0,"
            "  error_message TEXT DEFAULT ''"
            ")",

            "CREATE TABLE IF NOT EXISTS chunks ("
            "  hash TEXT PRIMARY KEY,"
            "  storage_path TEXT NOT NULL,"
            "  original_size INTEGER,"
            "  stored_size INTEGER,"
            "  compression INTEGER DEFAULT 0,"
            "  encrypted INTEGER DEFAULT 0,"
            "  ref_count INTEGER DEFAULT 1"
            ")",

            "CREATE TABLE IF NOT EXISTS file_manifests ("
            "  manifest_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  job_id INTEGER NOT NULL,"
            "  file_path TEXT NOT NULL,"
            "  file_name TEXT NOT NULL,"
            "  file_size INTEGER,"
            "  modified_time INTEGER,"
            "  file_hash TEXT,"
            "  FOREIGN KEY (job_id) REFERENCES jobs(job_id)"
            ")",

            "CREATE TABLE IF NOT EXISTS file_chunks ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  manifest_id INTEGER NOT NULL,"
            "  chunk_hash TEXT NOT NULL,"
            "  chunk_index INTEGER,"
            "  offset INTEGER,"
            "  size INTEGER,"
            "  deduplicated INTEGER DEFAULT 0,"
            "  FOREIGN KEY (manifest_id) REFERENCES file_manifests(manifest_id)"
            ")",

            "CREATE TABLE IF NOT EXISTS encryption_keys ("
            "  job_id INTEGER PRIMARY KEY,"
            "  key_hex TEXT NOT NULL,"
            "  FOREIGN KEY (job_id) REFERENCES jobs(job_id)"
            ")",

            "CREATE TABLE IF NOT EXISTS job_dependencies ("
            "  job_id INTEGER,"
            "  depends_on INTEGER,"
            "  PRIMARY KEY (job_id, depends_on),"
            "  FOREIGN KEY (job_id) REFERENCES jobs(job_id),"
            "  FOREIGN KEY (depends_on) REFERENCES jobs(job_id)"
            ")",

            "CREATE TABLE IF NOT EXISTS channels ("
            "  channel_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  name TEXT UNIQUE NOT NULL,"
            "  created_at INTEGER"
            ")",

            "CREATE TABLE IF NOT EXISTS messages ("
            "  msg_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  channel_name TEXT NOT NULL,"
            "  sender TEXT NOT NULL,"
            "  content TEXT,"
            "  msg_type TEXT DEFAULT 'text',"
            "  created_at INTEGER"
            ")",

            "CREATE INDEX IF NOT EXISTS idx_jobs_status ON jobs(status)",
            "CREATE INDEX IF NOT EXISTS idx_chunks_hash ON chunks(hash)",
            "CREATE INDEX IF NOT EXISTS idx_file_manifests_job ON file_manifests(job_id)",
            "CREATE INDEX IF NOT EXISTS idx_file_chunks_manifest ON file_chunks(manifest_id)",
            "CREATE INDEX IF NOT EXISTS idx_messages_channel ON messages(channel_name, created_at)",
        };

        for (auto& sql : schemas) {
            if (!exec_simple(sql)) {
                exec_simple("ROLLBACK");
                return false;
            }
        }
        exec_simple("COMMIT");
        LOG_INFO("Database tables initialized");
        return true;
    }

    bool increment_chunk_ref(const std::string& hash_hex) {
        Statement stmt;
        if (!stmt.prepare(db_, "UPDATE chunks SET ref_count = ref_count + 1 WHERE hash=?")) return false;
        stmt.bind_text(1, hash_hex);
        return stmt.step() == SQLITE_DONE;
    }

    BackupJob row_to_job(Statement& stmt) {
        BackupJob j;
        j.job_id          = stmt.column_int(0);
        j.source_path     = stmt.column_text(1);
        j.backup_name     = stmt.column_text(2);
        j.status          = static_cast<JobStatus>(stmt.column_int(3));
        j.priority        = static_cast<JobPriority>(stmt.column_int(4));
        j.compression     = static_cast<CompressionType>(stmt.column_int(5));
        j.encrypt         = stmt.column_int(6) != 0;
        j.incremental     = stmt.column_int(7) != 0;
        j.parent_job_id   = stmt.column_int(8);
        j.created_at      = static_cast<uint64_t>(stmt.column_int64(9));
        j.started_at      = static_cast<uint64_t>(stmt.column_int64(10));
        j.completed_at    = static_cast<uint64_t>(stmt.column_int64(11));
        j.total_bytes     = static_cast<uint64_t>(stmt.column_int64(12));
        j.processed_bytes = static_cast<uint64_t>(stmt.column_int64(13));
        j.stored_bytes    = static_cast<uint64_t>(stmt.column_int64(14));
        j.dedup_savings   = static_cast<uint64_t>(stmt.column_int64(15));
        j.file_count      = stmt.column_int(16);
        j.error_message   = stmt.column_text(17);
        return j;
    }
};

} // namespace ecpb
