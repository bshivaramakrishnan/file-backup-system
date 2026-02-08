#pragma once

#include <vector>
#include <optional>
#include <mutex>
#include <cstddef>

namespace ecpb {

template<typename T>
class CircularBuffer {
public:
    explicit CircularBuffer(size_t capacity = 1024)
        : buf_(capacity), capacity_(capacity), head_(0), tail_(0), count_(0) {}

    bool push(const T& item) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (count_ == capacity_) return false;  // full
        buf_[tail_] = item;
        tail_ = (tail_ + 1) % capacity_;
        ++count_;
        return true;
    }

    // Push with overwrite on full
    void push_overwrite(const T& item) {
        std::lock_guard<std::mutex> lock(mtx_);
        buf_[tail_] = item;
        tail_ = (tail_ + 1) % capacity_;
        if (count_ == capacity_) {
            head_ = (head_ + 1) % capacity_;  // overwrite oldest
        } else {
            ++count_;
        }
    }

    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (count_ == 0) return std::nullopt;
        T item = buf_[head_];
        head_ = (head_ + 1) % capacity_;
        --count_;
        return item;
    }

    std::optional<T> peek() const {
        std::lock_guard<std::mutex> lock(mtx_);
        if (count_ == 0) return std::nullopt;
        return buf_[head_];
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return count_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return count_ == 0;
    }

    bool full() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return count_ == capacity_;
    }

    size_t capacity() const { return capacity_; }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        head_ = tail_ = count_ = 0;
    }

    // Get last N items (most recent)
    std::vector<T> last_n(size_t n) const {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<T> result;
        size_t actual = (n < count_) ? n : count_;
        size_t start = (head_ + count_ - actual) % capacity_;
        for (size_t i = 0; i < actual; ++i) {
            result.push_back(buf_[(start + i) % capacity_]);
        }
        return result;
    }

private:
    std::vector<T> buf_;
    size_t capacity_;
    size_t head_;
    size_t tail_;
    size_t count_;
    mutable std::mutex mtx_;
};

} // namespace ecpb
