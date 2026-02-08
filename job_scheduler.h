#pragma once

#include "common/types.h"
#include "common/logger.h"
#include "storage/database.h"
#include "datastructures/priority_queue.h"
#include "datastructures/dag.h"

#include <mutex>
#include <vector>
#include <unordered_set>

namespace ecpb {

class JobScheduler {
public:
    explicit JobScheduler(Database& db) : db_(db) {}

    // Submit a new job, returns job_id
    int submit_job(BackupJob& job) {
        std::lock_guard<std::mutex> lock(mtx_);
        int job_id = db_.create_job(job);
        if (job_id < 0) {
            LOG_ERR("JobScheduler: failed to insert job into database");
            return -1;
        }
        job.job_id = job_id;

        // Add to priority queue
        JobEntry entry;
        entry.job_id = job_id;
        entry.priority = job.priority;
        entry.created_at = now_epoch_ms();
        pq_.push(entry);

        // Add to DAG
        dep_graph_.add_node(job_id);

        // Add dependencies from the job
        for (int dep : job.dependencies) {
            add_dependency_internal(job_id, dep);
        }

        LOG_INFO("Scheduler: submitted job %d [%s] priority=%s",
                 job_id, job.backup_name.c_str(), job_priority_str(job.priority));
        return job_id;
    }

    // Add dependency: job_id depends on depends_on
    bool add_dependency(int job_id, int depends_on) {
        std::lock_guard<std::mutex> lock(mtx_);
        return add_dependency_internal(job_id, depends_on);
    }

    // Get jobs that are ready to execute (all dependencies satisfied)
    std::vector<BackupJob> get_ready_jobs() {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<BackupJob> ready;
        auto ready_nodes = dep_graph_.get_ready_nodes();

        for (int node_id : ready_nodes) {
            // Must be PENDING
            auto job = db_.get_job(node_id);
            if (!job || job->status != JobStatus::PENDING) continue;
            if (in_progress_.count(node_id)) continue;

            ready.push_back(*job);
            in_progress_.insert(node_id);
        }

        // Sort by priority (higher first), then by creation time
        std::sort(ready.begin(), ready.end(), [](const BackupJob& a, const BackupJob& b) {
            if (a.priority != b.priority)
                return static_cast<int>(a.priority) > static_cast<int>(b.priority);
            return a.created_at < b.created_at;
        });

        return ready;
    }

    // Mark job as completed - removes from DAG to unblock dependents
    void mark_completed(int job_id) {
        std::lock_guard<std::mutex> lock(mtx_);
        dep_graph_.remove_node(job_id);
        in_progress_.erase(job_id);
        // Remove from priority queue
        pq_.remove_if([job_id](const JobEntry& e) { return e.job_id == job_id; });
        LOG_INFO("Scheduler: job %d marked completed", job_id);
    }

    // Mark job as failed
    void mark_failed(int job_id) {
        std::lock_guard<std::mutex> lock(mtx_);
        db_.update_job_status(job_id, JobStatus::FAILED, "Worker process failed");
        // Remove dependents (they can't proceed)
        auto dependents = dep_graph_.get_dependents(job_id);
        for (int dep : dependents) {
            db_.update_job_status(dep, JobStatus::CANCELLED,
                                 "Dependency job " + std::to_string(job_id) + " failed");
        }
        dep_graph_.remove_node(job_id);
        in_progress_.erase(job_id);
        pq_.remove_if([job_id](const JobEntry& e) { return e.job_id == job_id; });
    }

    size_t pending_count() {
        std::lock_guard<std::mutex> lock(mtx_);
        return pq_.size();
    }

private:
    Database& db_;
    mutable std::mutex mtx_;

    struct JobEntry {
        int job_id;
        JobPriority priority;
        uint64_t created_at;
    };

    // Compare: higher priority first, then older first
    struct JobCompare {
        bool operator()(const JobEntry& a, const JobEntry& b) const {
            if (a.priority != b.priority)
                return static_cast<int>(a.priority) < static_cast<int>(b.priority);
            return a.created_at > b.created_at;
        }
    };

    PriorityQueue<JobEntry, JobCompare> pq_;
    DAG<int> dep_graph_;
    std::unordered_set<int> in_progress_;

    bool add_dependency_internal(int job_id, int depends_on) {
        if (!dep_graph_.add_edge(depends_on, job_id)) {
            LOG_WARN("Scheduler: cannot add dependency %d -> %d (would create cycle)", depends_on, job_id);
            return false;
        }
        db_.add_dependency(job_id, depends_on);
        LOG_DEBUG("Scheduler: job %d depends on %d", job_id, depends_on);
        return true;
    }
};

} // namespace ecpb
