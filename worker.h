#pragma once

#include "common/types.h"
#include "common/logger.h"
#include "storage/chunk_store.h"
#include "storage/database.h"
#include "backup/snapshot.h"
#include "ipc/ipc.h"

#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace ecpb {

class BackupWorker {
public:
    struct Result {
        int job_id = -1;
        bool success = false;
        uint64_t total_bytes = 0;
        uint64_t stored_bytes = 0;
        uint64_t dedup_savings = 0;
        int file_count = 0;
        std::string error;
    };

    BackupWorker(Database& db, ChunkStore& store, SnapshotManager& snap_mgr)
        : db_(db), store_(store), snap_mgr_(snap_mgr) {}

    // Execute a backup job. Returns result struct.
    Result execute(BackupJob& job, const AES256::Key& aes_key,
                   MessageQueue* msg_queue = nullptr) {
        Result result;
        result.job_id = job.job_id;

        LOG_INFO("Worker[%d]: starting backup job %d for %s",
                 getpid(), job.job_id, job.source_path.c_str());

        // Update status to RUNNING
        db_.update_job_status(job.job_id, JobStatus::RUNNING);
        if (msg_queue) send_progress(msg_queue, job.job_id, IPCMessageType::JOB_START, 0, 0);

        // Create snapshot for consistent view
        SnapshotInfo snap = snap_mgr_.create_snapshot(job.job_id, job.source_path);
        if (!snap.is_consistent) {
            result.error = "Failed to create snapshot";
            db_.update_job_status(job.job_id, JobStatus::FAILED, result.error);
            if (msg_queue) send_progress(msg_queue, job.job_id, IPCMessageType::JOB_FAILED, 0, 0);
            return result;
        }

        // List all files in the snapshot
        std::vector<std::string> files = snap_mgr_.list_files(snap);
        result.file_count = static_cast<int>(files.size());

        if (files.empty()) {
            LOG_WARN("Worker[%d]: no files found in %s", getpid(), job.source_path.c_str());
        }

        // Compute total bytes
        for (auto& f : files) {
            struct stat st;
            if (stat(f.c_str(), &st) == 0) {
                result.total_bytes += static_cast<uint64_t>(st.st_size);
            }
        }

        // Process each file
        uint64_t processed = 0;
        std::string snap_base = snap.snapshot_path;
        if (!snap_base.empty() && snap_base.back() != '/') snap_base += '/';

        for (size_t i = 0; i < files.size(); ++i) {
            auto& file_path = files[i];
            struct stat st;
            if (stat(file_path.c_str(), &st) != 0) continue;

            // Compute relative path from snapshot directory
            std::string rel_path;
            if (file_path.substr(0, snap_base.size()) == snap_base) {
                rel_path = file_path.substr(snap_base.size());
            } else {
                rel_path = file_path;
                auto pos = rel_path.rfind('/');
                if (pos != std::string::npos) rel_path = rel_path.substr(pos + 1);
            }

            FileManifest manifest = store_.store_file(
                file_path, job.compression, job.encrypt, aes_key, job.job_id, rel_path);

            // Tally stats
            for (auto& chunk : manifest.chunks) {
                if (chunk.deduplicated) {
                    result.dedup_savings += chunk.size;
                } else {
                    auto meta = db_.get_chunk_meta(chunk.hash.str());
                    if (meta) {
                        result.stored_bytes += meta->stored_size;
                    }
                }
            }

            processed += manifest.file_size;

            // Send progress
            if (msg_queue) {
                send_progress(msg_queue, job.job_id, IPCMessageType::JOB_PROGRESS,
                              processed, result.total_bytes);
            }
        }

        // Store encryption key
        if (job.encrypt) {
            db_.store_encryption_key(job.job_id, AES256::key_to_hex(aes_key));
        }

        // Update final stats
        db_.update_job_stats(job.job_id, result.total_bytes, processed,
                            result.stored_bytes, result.dedup_savings, result.file_count);
        db_.update_job_status(job.job_id, JobStatus::COMPLETED);

        // Cleanup snapshot
        snap_mgr_.remove_snapshot(snap);

        result.success = true;
        if (msg_queue) send_progress(msg_queue, job.job_id, IPCMessageType::JOB_COMPLETE,
                                     processed, result.total_bytes);

        LOG_INFO("Worker[%d]: job %d completed - %d files, %s stored, %s dedup savings",
                 getpid(), job.job_id, result.file_count,
                 format_bytes(result.stored_bytes).c_str(),
                 format_bytes(result.dedup_savings).c_str());
        return result;
    }

private:
    Database& db_;
    ChunkStore& store_;
    SnapshotManager& snap_mgr_;

    void send_progress(MessageQueue* mq, int job_id, IPCMessageType type,
                       uint64_t v1, uint64_t v2) {
        IPCMessage msg{};
        msg.mtype = 1;
        msg.type = type;
        msg.job_id = job_id;
        msg.worker_pid = getpid();
        msg.value1 = v1;
        msg.value2 = v2;
        mq->send(msg);
    }
};

} // namespace ecpb
