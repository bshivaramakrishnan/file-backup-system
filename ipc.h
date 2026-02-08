#pragma once

#include "common/types.h"
#include "common/logger.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <cerrno>

namespace ecpb {

// ─── Shared Memory Segment ──────────────────────────────────────────
class SharedMemory {
public:
    SharedMemory() : fd_(-1), ptr_(nullptr), size_(0) {}

    ~SharedMemory() { destroy(); }

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    bool create(const std::string& name, size_t size) {
        name_ = "/" + name;
        size_ = size;

        // Remove any stale segment
        shm_unlink(name_.c_str());

        fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) {
            LOG_ERR("SharedMemory: shm_open create failed: %s", strerror(errno));
            return false;
        }
        if (ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            LOG_ERR("SharedMemory: ftruncate failed: %s", strerror(errno));
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        ptr_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (ptr_ == MAP_FAILED) {
            LOG_ERR("SharedMemory: mmap failed: %s", strerror(errno));
            ptr_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        std::memset(ptr_, 0, size);
        LOG_DEBUG("SharedMemory: created %s (%zu bytes)", name_.c_str(), size);
        return true;
    }

    bool open(const std::string& name, size_t size) {
        name_ = "/" + name;
        size_ = size;
        fd_ = shm_open(name_.c_str(), O_RDWR, 0666);
        if (fd_ < 0) {
            LOG_ERR("SharedMemory: shm_open failed: %s", strerror(errno));
            return false;
        }
        ptr_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (ptr_ == MAP_FAILED) {
            LOG_ERR("SharedMemory: mmap failed: %s", strerror(errno));
            ptr_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }

    void destroy() {
        if (ptr_ && ptr_ != MAP_FAILED) {
            munmap(ptr_, size_);
            ptr_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (!name_.empty()) {
            shm_unlink(name_.c_str());
            name_.clear();
        }
    }

    void* data() { return ptr_; }
    const void* data() const { return ptr_; }
    size_t size() const { return size_; }
    bool is_valid() const { return ptr_ != nullptr && ptr_ != MAP_FAILED; }

    // Write structured data at offset
    template<typename T>
    bool write_at(size_t offset, const T& val) {
        if (offset + sizeof(T) > size_) return false;
        std::memcpy(static_cast<uint8_t*>(ptr_) + offset, &val, sizeof(T));
        return true;
    }

    template<typename T>
    bool read_at(size_t offset, T& val) const {
        if (offset + sizeof(T) > size_) return false;
        std::memcpy(&val, static_cast<const uint8_t*>(ptr_) + offset, sizeof(T));
        return true;
    }

private:
    std::string name_;
    int fd_;
    void* ptr_;
    size_t size_;
};

// ─── Progress region in shared memory ────────────────────────────────
struct WorkerProgress {
    std::atomic<int>      job_id;
    std::atomic<int>      worker_pid;
    std::atomic<uint64_t> bytes_processed;
    std::atomic<uint64_t> bytes_total;
    std::atomic<int>      files_done;
    std::atomic<int>      files_total;
    std::atomic<int>      status;  // JobStatus cast to int
    char                  current_file[256];
};

// ─── POSIX Message Queue (pipe-based fallback) ──────────────────────
// Using a pipe-based approach since POSIX mq may not be available
class MessageQueue {
public:
    MessageQueue() : read_fd_(-1), write_fd_(-1) {}
    ~MessageQueue() { destroy(); }

    MessageQueue(const MessageQueue&) = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;

    bool create(const std::string& name) {
        name_ = name;
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            LOG_ERR("MessageQueue: pipe failed: %s", strerror(errno));
            return false;
        }
        read_fd_ = pipefd[0];
        write_fd_ = pipefd[1];
        LOG_DEBUG("MessageQueue: created %s", name_.c_str());
        return true;
    }

    bool send(const IPCMessage& msg) {
        if (write_fd_ < 0) return false;
        ssize_t written = ::write(write_fd_, &msg, sizeof(msg));
        return written == static_cast<ssize_t>(sizeof(msg));
    }

    bool receive(IPCMessage& msg, int timeout_ms = 1000) {
        if (read_fd_ < 0) return false;

        // Use select for timeout
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(read_fd_, &readfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ret = select(read_fd_ + 1, &readfds, nullptr, nullptr, &tv);
        if (ret <= 0) return false;

        ssize_t n = ::read(read_fd_, &msg, sizeof(msg));
        return n == static_cast<ssize_t>(sizeof(msg));
    }

    void destroy() {
        if (read_fd_ >= 0) { ::close(read_fd_); read_fd_ = -1; }
        if (write_fd_ >= 0) { ::close(write_fd_); write_fd_ = -1; }
    }

    int read_fd() const { return read_fd_; }
    int write_fd() const { return write_fd_; }

    // For fork: child closes write end, parent closes read end, etc.
    void close_read() { if (read_fd_ >= 0) { ::close(read_fd_); read_fd_ = -1; } }
    void close_write() { if (write_fd_ >= 0) { ::close(write_fd_); write_fd_ = -1; } }

private:
    std::string name_;
    int read_fd_;
    int write_fd_;
};

// ─── Named Semaphore ─────────────────────────────────────────────────
class NamedSemaphore {
public:
    NamedSemaphore() : sem_(SEM_FAILED) {}
    ~NamedSemaphore() { destroy(); }

    NamedSemaphore(const NamedSemaphore&) = delete;
    NamedSemaphore& operator=(const NamedSemaphore&) = delete;

    bool create(const std::string& name, unsigned int initial_value) {
        name_ = "/" + name;
        sem_unlink(name_.c_str());
        sem_ = sem_open(name_.c_str(), O_CREAT | O_EXCL, 0666, initial_value);
        if (sem_ == SEM_FAILED) {
            LOG_ERR("Semaphore: sem_open failed: %s", strerror(errno));
            return false;
        }
        LOG_DEBUG("Semaphore: created %s (value=%u)", name_.c_str(), initial_value);
        return true;
    }

    bool open(const std::string& name) {
        name_ = "/" + name;
        sem_ = sem_open(name_.c_str(), 0);
        return sem_ != SEM_FAILED;
    }

    bool wait() {
        if (sem_ == SEM_FAILED) return false;
        return sem_wait(sem_) == 0;
    }

    bool try_wait() {
        if (sem_ == SEM_FAILED) return false;
        return sem_trywait(sem_) == 0;
    }

    bool post() {
        if (sem_ == SEM_FAILED) return false;
        return sem_post(sem_) == 0;
    }

    void destroy() {
        if (sem_ != SEM_FAILED) {
            sem_close(sem_);
            sem_ = SEM_FAILED;
        }
        if (!name_.empty()) {
            sem_unlink(name_.c_str());
            name_.clear();
        }
    }

private:
    std::string name_;
    sem_t* sem_;
};

} // namespace ecpb
