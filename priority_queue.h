#pragma once

#include <vector>
#include <functional>
#include <stdexcept>
#include <optional>

namespace ecpb {

template<typename T, typename Compare = std::greater<T>>
class PriorityQueue {
public:
    explicit PriorityQueue(Compare cmp = Compare()) : cmp_(cmp) {}

    void push(const T& item) {
        heap_.push_back(item);
        sift_up(heap_.size() - 1);
    }

    T pop() {
        if (heap_.empty()) throw std::runtime_error("PriorityQueue: pop on empty queue");
        T top = heap_[0];
        heap_[0] = heap_.back();
        heap_.pop_back();
        if (!heap_.empty()) sift_down(0);
        return top;
    }

    const T& top() const {
        if (heap_.empty()) throw std::runtime_error("PriorityQueue: top on empty queue");
        return heap_[0];
    }

    bool   empty() const { return heap_.empty(); }
    size_t size()  const { return heap_.size(); }

    // Remove first element matching predicate
    template<typename Pred>
    bool remove_if(Pred pred) {
        for (size_t i = 0; i < heap_.size(); ++i) {
            if (pred(heap_[i])) {
                heap_[i] = heap_.back();
                heap_.pop_back();
                if (i < heap_.size()) {
                    sift_down(i);
                    sift_up(i);
                }
                return true;
            }
        }
        return false;
    }

    // Update priority: find matching element, replace it, re-heapify
    template<typename Pred>
    bool update(Pred pred, const T& new_val) {
        for (size_t i = 0; i < heap_.size(); ++i) {
            if (pred(heap_[i])) {
                heap_[i] = new_val;
                sift_down(i);
                sift_up(i);
                return true;
            }
        }
        return false;
    }

    void clear() { heap_.clear(); }

    // Iterate (not in priority order)
    template<typename Fn>
    void for_each(Fn fn) const {
        for (auto& item : heap_) fn(item);
    }

private:
    std::vector<T> heap_;
    Compare cmp_;

    void sift_up(size_t idx) {
        while (idx > 0) {
            size_t parent = (idx - 1) / 2;
            if (cmp_(heap_[parent], heap_[idx])) {
                std::swap(heap_[parent], heap_[idx]);
                idx = parent;
            } else break;
        }
    }

    void sift_down(size_t idx) {
        size_t n = heap_.size();
        while (true) {
            size_t best = idx;
            size_t left  = 2 * idx + 1;
            size_t right = 2 * idx + 2;
            if (left < n  && cmp_(heap_[best], heap_[left]))  best = left;
            if (right < n && cmp_(heap_[best], heap_[right])) best = right;
            if (best == idx) break;
            std::swap(heap_[idx], heap_[best]);
            idx = best;
        }
    }
};

} // namespace ecpb
