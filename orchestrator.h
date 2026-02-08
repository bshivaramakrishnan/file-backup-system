#pragma once

#include "common/types.h"
#include "common/logger.h"
#include "crypto/aes256.h"
#include "ipc/ipc.h"
#include "storage/database.h"
#include "storage/chunk_store.h"
#include "backup/snapshot.h"
#include "backup/worker.h"
#include "scheduler/job_scheduler.h"

#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <map>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

namespace ecpb {

class BackupOrchestrator {
public:
    BackupOrchestrator(Database& db, const std::string& data_dir)
        : db_(db), data_dir_(data_dir),
          chunk_store_(db, data_dir + "/storage"),
          snap_mgr_(db, data_dir + "/snapshots"),
          scheduler_(db),
          running_(false), aes_key_(AES256::generate_key()) {
        LOG_INFO("BackupOrchestrator initialized with AES-256 key");
    }

    ~BackupOrchestrator() { stop(); }

    bool initialize() {
        // Create IPC primitives
        if (!shm_.create("ecpb_shm", SHM_SEGMENT_SIZE)) {
            LOG_ERR("Orchestrator: failed to create shared memory");
            return false;
        }
        if (!msg_queue_.create("ecpb_mq")) {
            LOG_ERR("Orchestrator: failed to create message queue");
            return false;
        }
        if (!worker_sem_.create("ecpb_worker_sem", MAX_WORKER_PROCESSES)) {
            LOG_ERR("Orchestrator: failed to create worker semaphore");
            return false;
        }
        return true;
    }

    // Submit a backup job
    int submit_job(const std::string& source_path, const std::string& name,
                   JobPriority priority = JobPriority::NORMAL,
                   CompressionType comp = CompressionType::LZ4,
                   bool encrypt = true, bool incremental = false) {
        BackupJob job;
        job.source_path = source_path;
        job.backup_name = name;
        job.priority = priority;
        job.compression = comp;
        job.encrypt = encrypt;
        job.incremental = incremental;
        return scheduler_.submit_job(job);
    }

    // Add dependency between jobs
    bool add_dependency(int job_id, int depends_on) {
        return scheduler_.add_dependency(job_id, depends_on);
    }

    // Run the backup orchestrator (can be called directly without fork for simple mode)
    void run_single_threaded() {
        running_ = true;
        LOG_INFO("Orchestrator started (single-threaded mode)");

        while (running_) {
            auto ready = scheduler_.get_ready_jobs();
            if (ready.empty()) {
                // Check if we have pending jobs
                auto pending = db_.get_jobs_by_status(JobStatus::PENDING);
                if (pending.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            for (auto& job : ready) {
                if (!running_) break;
                execute_job_direct(job);
                scheduler_.mark_completed(job.job_id);
            }
        }

        LOG_INFO("Orchestrator stopped");
        running_ = false;
    }

    // Run with fork() for multi-process execution
    void run_multi_process() {
        running_ = true;
        LOG_INFO("Orchestrator started (multi-process mode)");

        while (running_) {
            // Reap finished children
            reap_children();

            // Process incoming messages from workers
            process_messages();

            auto ready = scheduler_.get_ready_jobs();
            if (ready.empty()) {
                auto pending = db_.get_jobs_by_status(JobStatus::PENDING);
                if (pending.empty() && active_workers_.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            for (auto& job : ready) {
                if (!running_) break;
                if (static_cast<int>(active_workers_.size()) >= MAX_WORKER_PROCESSES) break;

                // Try to acquire semaphore (non-blocking)
                if (!worker_sem_.try_wait()) break;

                fork_worker(job);
            }
        }

        // Wait for all children
        while (!active_workers_.empty()) {
            reap_children();
            process_messages();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        LOG_INFO("Orchestrator stopped");
        running_ = false;
    }

    void stop() {
        running_ = false;
    }

    // Getters
    Database& database() { return db_; }
    ChunkStore& chunk_store() { return chunk_store_; }
    const AES256::Key& aes_key() const { return aes_key_; }
    void set_aes_key(const AES256::Key& key) { aes_key_ = key; }

    int active_worker_count() const { return static_cast<int>(active_workers_.size()); }

private:
    Database& db_;
    std::string data_dir_;
    ChunkStore chunk_store_;
    SnapshotManager snap_mgr_;
    JobScheduler scheduler_;

    SharedMemory shm_;
    MessageQueue msg_queue_;
    NamedSemaphore worker_sem_;

    std::atomic<bool> running_;
    AES256::Key aes_key_;

    struct WorkerInfo {
        int job_id;
        pid_t pid;
        uint64_t start_time;
    };
    std::map<pid_t, WorkerInfo> active_workers_;

    void execute_job_direct(BackupJob& job) {
        BackupWorker worker(db_, chunk_store_, snap_mgr_);
        auto result = worker.execute(job, aes_key_, nullptr);
        if (!result.success) {
            LOG_ERR("Job %d failed: %s", job.job_id, result.error.c_str());
        }
    }

    void fork_worker(BackupJob& job) {
        db_.update_job_status(job.job_id, JobStatus::RUNNING);

        pid_t pid = fork();
        if (pid < 0) {
            LOG_ERR("Orchestrator: fork() failed: %s", strerror(errno));
            worker_sem_.post();
            return;
        }

        if (pid == 0) {
            // ─── Child process ───
            // Re-open database in child (SQLite requires this after fork)
            Database child_db;
            if (!child_db.open(data_dir_ + "/ecpb.db")) {
                _exit(1);
            }
            ChunkStore child_store(child_db, data_dir_ + "/storage");
            SnapshotManager child_snap(child_db, data_dir_ + "/snapshots");
            BackupWorker worker(child_db, child_store, child_snap);

            auto result = worker.execute(job, aes_key_, &msg_queue_);
            child_db.close();
            _exit(result.success ? 0 : 1);
        }

        // ─── Parent process ───
        WorkerInfo winfo;
        winfo.job_id = job.job_id;
        winfo.pid = pid;
        winfo.start_time = now_epoch_ms();
        active_workers_[pid] = winfo;

        LOG_INFO("Forked worker PID %d for job %d", pid, job.job_id);
    }

    void reap_children() {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            auto it = active_workers_.find(pid);
            if (it != active_workers_.end()) {
                int job_id = it->second.job_id;
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    scheduler_.mark_completed(job_id);
                    LOG_INFO("Worker PID %d (job %d) exited successfully", pid, job_id);
                } else {
                    scheduler_.mark_failed(job_id);
                    LOG_ERR("Worker PID %d (job %d) failed", pid, job_id);
                }
                active_workers_.erase(it);
                worker_sem_.post();
            }
        }
    }

    void process_messages() {
        IPCMessage msg;
        while (msg_queue_.receive(msg, 0)) {
            switch (msg.type) {
                case IPCMessageType::JOB_PROGRESS:
                    LOG_DEBUG("Job %d progress: %s / %s",
                             msg.job_id,
                             format_bytes(msg.value1).c_str(),
                             format_bytes(msg.value2).c_str());
                    break;
                case IPCMessageType::JOB_COMPLETE:
                    LOG_INFO("Job %d reports completion via IPC", msg.job_id);
                    break;
                case IPCMessageType::JOB_FAILED:
                    LOG_ERR("Job %d reports failure via IPC", msg.job_id);
                    break;
                default:
                    break;
            }
        }
    }
};

} // namespace ecpb
