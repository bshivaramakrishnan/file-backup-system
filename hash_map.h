#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <stdexcept>

namespace ecpb {

template<typename K, typename V>
class HashMap {
public:
    explicit HashMap(size_t initial_cap = 256)
        : size_(0), capacity_(next_pow2(initial_cap)),
          buckets_(capacity_), states_(capacity_, State::EMPTY) {}

    void insert(const K& key, const V& value) {
        if (load_factor() > 0.7) rehash(capacity_ * 2);
        size_t idx = probe(key);
        if (states_[idx] == State::OCCUPIED && buckets_[idx].first == key) {
            buckets_[idx].second = value;
            return;
        }
        buckets_[idx] = {key, value};
        states_[idx] = State::OCCUPIED;
        ++size_;
    }

    std::optional<V> find(const K& key) const {
        size_t idx = find_idx(key);
        if (idx == capacity_) return std::nullopt;
        return buckets_[idx].second;
    }

    bool contains(const K& key) const {
        return find_idx(key) != capacity_;
    }

    bool erase(const K& key) {
        size_t idx = find_idx(key);
        if (idx == capacity_) return false;
        states_[idx] = State::DELETED;
        --size_;
        return true;
    }

    size_t size() const { return size_; }
    bool   empty() const { return size_ == 0; }
    void   clear() {
        std::fill(states_.begin(), states_.end(), State::EMPTY);
        size_ = 0;
    }

    // Iterate all occupied entries
    template<typename Fn>
    void for_each(Fn fn) const {
        for (size_t i = 0; i < capacity_; ++i) {
            if (states_[i] == State::OCCUPIED) {
                fn(buckets_[i].first, buckets_[i].second);
            }
        }
    }

private:
    enum class State : uint8_t { EMPTY, OCCUPIED, DELETED };

    size_t size_;
    size_t capacity_;
    std::vector<std::pair<K,V>> buckets_;
    std::vector<State>          states_;

    double load_factor() const { return static_cast<double>(size_) / capacity_; }

    static size_t next_pow2(size_t n) {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    size_t hash_key(const K& key) const {
        return std::hash<K>{}(key) & (capacity_ - 1);
    }

    size_t probe(const K& key) const {
        size_t idx = hash_key(key);
        size_t first_deleted = capacity_;
        for (size_t i = 0; i < capacity_; ++i) {
            size_t pos = (idx + i) & (capacity_ - 1);
            if (states_[pos] == State::EMPTY) {
                return (first_deleted != capacity_) ? first_deleted : pos;
            }
            if (states_[pos] == State::DELETED && first_deleted == capacity_) {
                first_deleted = pos;
            }
            if (states_[pos] == State::OCCUPIED && buckets_[pos].first == key) {
                return pos;
            }
        }
        return (first_deleted != capacity_) ? first_deleted : 0;
    }

    size_t find_idx(const K& key) const {
        size_t idx = hash_key(key);
        for (size_t i = 0; i < capacity_; ++i) {
            size_t pos = (idx + i) & (capacity_ - 1);
            if (states_[pos] == State::EMPTY) return capacity_;
            if (states_[pos] == State::OCCUPIED && buckets_[pos].first == key) return pos;
        }
        return capacity_;
    }

    void rehash(size_t new_cap) {
        auto old_buckets = std::move(buckets_);
        auto old_states  = std::move(states_);
        size_t old_cap   = capacity_;

        capacity_ = new_cap;
        buckets_.assign(capacity_, {});
        states_.assign(capacity_, State::EMPTY);
        size_ = 0;

        for (size_t i = 0; i < old_cap; ++i) {
            if (old_states[i] == State::OCCUPIED) {
                insert(old_buckets[i].first, old_buckets[i].second);
            }
        }
    }
};

} // namespace ecpb
