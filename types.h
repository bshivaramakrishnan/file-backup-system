#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <atomic>
#include <mutex>
#include <algorithm>

namespace ecpb {

// ─── Constants ───────────────────────────────────────────────────────
constexpr size_t CHUNK_SIZE            = 64 * 1024;          // 64 KB
constexpr size_t MAX_FILE_SIZE         = 4ULL * 1024 * 1024 * 1024; // 4 GB
constexpr size_t SHA256_HEX_LEN       = 64;
constexpr size_t SHA256_BIN_LEN       = 32;
constexpr size_t AES_KEY_LEN          = 32;                  // AES-256
constexpr size_t AES_IV_LEN           = 16;
constexpr size_t AES_BLOCK_SIZE       = 16;
constexpr size_t ROLLING_WINDOW       = 48;
constexpr int    SQLITE_BUSY_TIMEOUT_MS = 5000;
constexpr int    SQLITE_MAX_RETRIES   = 10;
constexpr size_t SHM_SEGMENT_SIZE     = 4 * 1024 * 1024;    // 4 MB
constexpr int    MSG_QUEUE_MAX_MSG    = 8192;
constexpr size_t CIRCULAR_BUF_CAP     = 1024;
constexpr int    MAX_WORKER_PROCESSES = 4;
constexpr int    BPLUS_TREE_ORDER     = 64;

// ─── SHA-256 Hash ────────────────────────────────────────────────────
using HashDigest = std::array<uint8_t, SHA256_BIN_LEN>;

struct HashHex {
    char data[SHA256_HEX_LEN + 1] = {};
    const char* c_str() const { return data; }
    std::string str() const { return std::string(data, SHA256_HEX_LEN); }
    bool operator==(const HashHex& o) const { return std::memcmp(data, o.data, SHA256_HEX_LEN) == 0; }
    bool operator!=(const HashHex& o) const { return !(*this == o); }
    bool operator<(const HashHex& o)  const { return std::memcmp(data, o.data, SHA256_HEX_LEN) < 0; }
};

// ─── Chunk Descriptor ────────────────────────────────────────────────
struct ChunkInfo {
    HashHex    hash;
    uint64_t   offset       = 0;
    uint32_t   size         = 0;
    uint32_t   chunk_index  = 0;
    bool       deduplicated = false;
};

// ─── File Manifest ───────────────────────────────────────────────────
struct FileManifest {
    std::string              file_path;
    std::string              file_name;
    uint64_t                 file_size      = 0;
    uint64_t                 modified_time  = 0;
    HashHex                  file_hash;
    std::vector<ChunkInfo>   chunks;
};

// ─── Job Types ───────────────────────────────────────────────────────
enum class JobStatus : int {
    PENDING   = 0,
    RUNNING   = 1,
    COMPLETED = 2,
    FAILED    = 3,
    CANCELLED = 4
};

enum class JobPriority : int {
    LOW    = 0,
    NORMAL = 1,
    HIGH   = 2,
    URGENT = 3
};

enum class CompressionType : int {
    NONE = 0,
    LZ4  = 1,
    ZSTD = 2
};

inline const char* job_status_str(JobStatus s) {
    switch (s) {
        case JobStatus::PENDING:   return "PENDING";
        case JobStatus::RUNNING:   return "RUNNING";
        case JobStatus::COMPLETED: return "COMPLETED";
        case JobStatus::FAILED:    return "FAILED";
        case JobStatus::CANCELLED: return "CANCELLED";
    }
    return "UNKNOWN";
}

inline const char* job_priority_str(JobPriority p) {
    switch (p) {
        case JobPriority::LOW:    return "LOW";
        case JobPriority::NORMAL: return "NORMAL";
        case JobPriority::HIGH:   return "HIGH";
        case JobPriority::URGENT: return "URGENT";
    }
    return "UNKNOWN";
}

inline const char* compression_str(CompressionType c) {
    switch (c) {
        case CompressionType::NONE: return "NONE";
        case CompressionType::LZ4:  return "LZ4";
        case CompressionType::ZSTD: return "ZSTD";
    }
    return "UNKNOWN";
}

struct BackupJob {
    int              job_id          = -1;
    std::string      source_path;
    std::string      backup_name;
    JobStatus        status          = JobStatus::PENDING;
    JobPriority      priority        = JobPriority::NORMAL;
    CompressionType  compression     = CompressionType::LZ4;
    bool             encrypt         = true;
    bool             incremental     = false;
    int              parent_job_id   = -1;
    uint64_t         created_at      = 0;
    uint64_t         started_at      = 0;
    uint64_t         completed_at    = 0;
    uint64_t         total_bytes     = 0;
    uint64_t         processed_bytes = 0;
    uint64_t         stored_bytes    = 0;
    uint64_t         dedup_savings   = 0;
    int              file_count      = 0;
    std::string      error_message;
    std::vector<int> dependencies;
};

// ─── IPC Message Types ───────────────────────────────────────────────
enum class IPCMessageType : int {
    JOB_START       = 1,
    JOB_PROGRESS    = 2,
    JOB_COMPLETE    = 3,
    JOB_FAILED      = 4,
    CHUNK_STORED    = 5,
    SNAPSHOT_READY  = 6,
    SHUTDOWN        = 7,
    HEARTBEAT       = 8
};

struct IPCMessage {
    long           mtype = 1;
    IPCMessageType type;
    int            job_id     = 0;
    int            worker_pid = 0;
    uint64_t       value1     = 0;
    uint64_t       value2     = 0;
    char           payload[256] = {};
};

// ─── Snapshot ────────────────────────────────────────────────────────
struct SnapshotInfo {
    int         snapshot_id   = -1;
    int         job_id        = -1;
    std::string snapshot_path;
    uint64_t    created_at    = 0;
    bool        is_consistent = false;
};

// ─── Restore Request ─────────────────────────────────────────────────
struct RestoreRequest {
    int         job_id           = -1;
    std::string restore_path;
    bool        verify_integrity = true;
};

// ─── Timestamp helpers ───────────────────────────────────────────────
inline uint64_t now_epoch_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

inline std::string epoch_to_string(uint64_t epoch_ms) {
    auto tp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(epoch_ms));
    auto t = std::chrono::system_clock::to_time_t(tp);
    char buf[64];
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return std::string(buf);
}

inline std::string format_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double val = static_cast<double>(bytes);
    while (val >= 1024.0 && unit < 4) {
        val /= 1024.0;
        unit++;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f %s", val, units[unit]);
    return buf;
}

} // namespace ecpb
