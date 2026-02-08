#pragma once

#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <algorithm>
#include <functional>

namespace ecpb {

template<typename K, typename V, int ORDER = 64>
class BPlusTree {
public:
    BPlusTree() : root_(std::make_unique<LeafNode>()), size_(0) {}

    void insert(const K& key, const V& value) {
        auto result = insert_internal(root_.get(), key, value);
        if (result) {
            // Root was split; create new root
            auto new_root = std::make_unique<InternalNode>();
            new_root->keys.push_back(result->split_key);
            new_root->children.push_back(std::move(root_));
            new_root->children.push_back(std::move(result->new_node));
            root_ = std::move(new_root);
        }
        ++size_;
    }

    std::optional<V> find(const K& key) const {
        Node* node = root_.get();
        while (!node->is_leaf) {
            auto* internal = static_cast<InternalNode*>(node);
            int idx = upper_bound_idx(internal->keys, key);
            node = internal->children[idx].get();
        }
        auto* leaf = static_cast<LeafNode*>(node);
        for (size_t i = 0; i < leaf->keys.size(); ++i) {
            if (leaf->keys[i] == key) return leaf->values[i];
        }
        return std::nullopt;
    }

    bool contains(const K& key) const { return find(key).has_value(); }

    bool erase(const K& key) {
        bool found = erase_internal(root_.get(), key);
        if (found) --size_;
        // Simplify root if internal with one child
        if (!root_->is_leaf) {
            auto* internal = static_cast<InternalNode*>(root_.get());
            if (internal->children.size() == 1) {
                root_ = std::move(internal->children[0]);
            }
        }
        return found;
    }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    // In-order traversal of all key-value pairs
    template<typename Fn>
    void for_each(Fn fn) const {
        // Find leftmost leaf
        Node* node = root_.get();
        while (!node->is_leaf) {
            node = static_cast<InternalNode*>(node)->children[0].get();
        }
        auto* leaf = static_cast<LeafNode*>(node);
        while (leaf) {
            for (size_t i = 0; i < leaf->keys.size(); ++i) {
                fn(leaf->keys[i], leaf->values[i]);
            }
            leaf = leaf->next;
        }
    }

    // Range query [lo, hi]
    std::vector<std::pair<K,V>> range(const K& lo, const K& hi) const {
        std::vector<std::pair<K,V>> result;
        Node* node = root_.get();
        while (!node->is_leaf) {
            auto* internal = static_cast<InternalNode*>(node);
            int idx = upper_bound_idx(internal->keys, lo);
            node = internal->children[idx].get();
        }
        auto* leaf = static_cast<LeafNode*>(node);
        while (leaf) {
            for (size_t i = 0; i < leaf->keys.size(); ++i) {
                if (leaf->keys[i] > hi) return result;
                if (leaf->keys[i] >= lo) {
                    result.emplace_back(leaf->keys[i], leaf->values[i]);
                }
            }
            leaf = leaf->next;
        }
        return result;
    }

private:
    static constexpr int MAX_KEYS = ORDER - 1;
    static constexpr int MIN_KEYS = (ORDER - 1) / 2;

    struct Node {
        bool is_leaf;
        explicit Node(bool leaf) : is_leaf(leaf) {}
        virtual ~Node() = default;
    };

    struct LeafNode : Node {
        std::vector<K> keys;
        std::vector<V> values;
        LeafNode* next = nullptr;  // linked list for range queries
        LeafNode() : Node(true) {}
    };

    struct InternalNode : Node {
        std::vector<K> keys;
        std::vector<std::unique_ptr<Node>> children;
        InternalNode() : Node(false) {}
    };

    struct SplitResult {
        K split_key;
        std::unique_ptr<Node> new_node;
    };

    std::unique_ptr<Node> root_;
    size_t size_;

    static int upper_bound_idx(const std::vector<K>& keys, const K& key) {
        int lo = 0, hi = static_cast<int>(keys.size());
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (keys[mid] <= key) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    std::optional<SplitResult> insert_internal(Node* node, const K& key, const V& value) {
        if (node->is_leaf) {
            auto* leaf = static_cast<LeafNode*>(node);
            // Find insertion point
            auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
            size_t pos = it - leaf->keys.begin();
            // Update if key exists
            if (it != leaf->keys.end() && *it == key) {
                leaf->values[pos] = value;
                --size_;  // will be incremented by caller
                return std::nullopt;
            }
            leaf->keys.insert(leaf->keys.begin() + pos, key);
            leaf->values.insert(leaf->values.begin() + pos, value);

            if (static_cast<int>(leaf->keys.size()) > MAX_KEYS) {
                return split_leaf(leaf);
            }
            return std::nullopt;
        }

        auto* internal = static_cast<InternalNode*>(node);
        int idx = upper_bound_idx(internal->keys, key);
        auto result = insert_internal(internal->children[idx].get(), key, value);
        if (!result) return std::nullopt;

        // Insert the split key and new child
        internal->keys.insert(internal->keys.begin() + idx, result->split_key);
        internal->children.insert(internal->children.begin() + idx + 1, std::move(result->new_node));

        if (static_cast<int>(internal->keys.size()) > MAX_KEYS) {
            return split_internal(internal);
        }
        return std::nullopt;
    }

    SplitResult split_leaf(LeafNode* leaf) {
        auto new_leaf = std::make_unique<LeafNode>();
        int mid = static_cast<int>(leaf->keys.size()) / 2;

        new_leaf->keys.assign(leaf->keys.begin() + mid, leaf->keys.end());
        new_leaf->values.assign(leaf->values.begin() + mid, leaf->values.end());
        new_leaf->next = leaf->next;
        leaf->next = new_leaf.get();

        K split_key = new_leaf->keys[0];
        leaf->keys.resize(mid);
        leaf->values.resize(mid);

        return {split_key, std::move(new_leaf)};
    }

    SplitResult split_internal(InternalNode* node) {
        auto new_node = std::make_unique<InternalNode>();
        int mid = static_cast<int>(node->keys.size()) / 2;
        K split_key = node->keys[mid];

        new_node->keys.assign(node->keys.begin() + mid + 1, node->keys.end());
        new_node->children.reserve(new_node->keys.size() + 1);
        for (size_t i = mid + 1; i < node->children.size(); ++i) {
            new_node->children.push_back(std::move(node->children[i]));
        }
        node->keys.resize(mid);
        node->children.resize(mid + 1);

        return {split_key, std::move(new_node)};
    }

    bool erase_internal(Node* node, const K& key) {
        if (node->is_leaf) {
            auto* leaf = static_cast<LeafNode*>(node);
            auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
            if (it == leaf->keys.end() || *it != key) return false;
            size_t pos = it - leaf->keys.begin();
            leaf->keys.erase(leaf->keys.begin() + pos);
            leaf->values.erase(leaf->values.begin() + pos);
            return true;
        }
        auto* internal = static_cast<InternalNode*>(node);
        int idx = upper_bound_idx(internal->keys, key);
        return erase_internal(internal->children[idx].get(), key);
    }
};

} // namespace ecpb
