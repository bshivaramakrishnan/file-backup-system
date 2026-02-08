#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <stdexcept>
#include <algorithm>

namespace ecpb {

template<typename T>
class DAG {
public:
    void add_node(const T& node) {
        if (adj_.find(node) == adj_.end()) {
            adj_[node] = {};
            in_degree_[node] = 0;
        }
    }

    // Add edge: dependency -> dependent  (dependent depends on dependency)
    bool add_edge(const T& from, const T& to) {
        add_node(from);
        add_node(to);
        // Check for cycle: would adding from->to create a cycle?
        if (from == to) return false;
        if (has_path(to, from)) return false;

        adj_[from].insert(to);
        in_degree_[to]++;
        return true;
    }

    bool remove_edge(const T& from, const T& to) {
        auto it = adj_.find(from);
        if (it == adj_.end()) return false;
        if (it->second.erase(to) > 0) {
            in_degree_[to]--;
            return true;
        }
        return false;
    }

    void remove_node(const T& node) {
        // Remove all edges to this node
        for (auto& [src, dests] : adj_) {
            if (dests.erase(node) > 0) {
                // in_degree_ for node reduced but we're removing it anyway
            }
        }
        // Remove all edges from this node
        if (adj_.count(node)) {
            for (auto& dest : adj_[node]) {
                in_degree_[dest]--;
            }
        }
        adj_.erase(node);
        in_degree_.erase(node);
    }

    // Topological sort (Kahn's algorithm)
    std::vector<T> topological_sort() const {
        std::unordered_map<T, int> deg = in_degree_;
        std::queue<T> q;
        for (auto& [node, d] : deg) {
            if (d == 0) q.push(node);
        }
        std::vector<T> result;
        while (!q.empty()) {
            T node = q.front(); q.pop();
            result.push_back(node);
            auto it = adj_.find(node);
            if (it != adj_.end()) {
                for (auto& next : it->second) {
                    if (--deg[next] == 0) q.push(next);
                }
            }
        }
        if (result.size() != adj_.size()) {
            throw std::runtime_error("DAG: cycle detected");
        }
        return result;
    }

    // Get all nodes with no remaining dependencies (in_degree == 0)
    std::vector<T> get_ready_nodes() const {
        std::vector<T> ready;
        for (auto& [node, d] : in_degree_) {
            if (d == 0) ready.push_back(node);
        }
        return ready;
    }

    // Get direct dependencies of a node
    std::vector<T> get_dependencies(const T& node) const {
        std::vector<T> deps;
        for (auto& [src, dests] : adj_) {
            if (dests.count(node)) deps.push_back(src);
        }
        return deps;
    }

    // Get direct dependents of a node
    std::vector<T> get_dependents(const T& node) const {
        auto it = adj_.find(node);
        if (it == adj_.end()) return {};
        return std::vector<T>(it->second.begin(), it->second.end());
    }

    bool has_node(const T& node) const { return adj_.count(node) > 0; }
    size_t node_count() const { return adj_.size(); }
    bool empty() const { return adj_.empty(); }

    std::vector<T> get_all_nodes() const {
        std::vector<T> nodes;
        for (auto& [n, _] : adj_) nodes.push_back(n);
        return nodes;
    }

private:
    std::unordered_map<T, std::unordered_set<T>> adj_;
    std::unordered_map<T, int> in_degree_;

    // BFS path check for cycle detection
    bool has_path(const T& from, const T& to) const {
        std::unordered_set<T> visited;
        std::queue<T> q;
        q.push(from);
        while (!q.empty()) {
            T cur = q.front(); q.pop();
            if (cur == to) return true;
            if (visited.count(cur)) continue;
            visited.insert(cur);
            auto it = adj_.find(cur);
            if (it != adj_.end()) {
                for (auto& next : it->second) q.push(next);
            }
        }
        return false;
    }
};

} // namespace ecpb
