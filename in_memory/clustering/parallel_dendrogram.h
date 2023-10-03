// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef THIRD_PARTY_GRAPH_MINING_IN_MEMORY_CLUSTERING_PARALLEL_DENDROGRAM_H_
#define THIRD_PARTY_GRAPH_MINING_IN_MEMORY_CLUSTERING_PARALLEL_DENDROGRAM_H_

#include <limits>
#include <utility>

#include "absl/log/absl_check.h"
#include "gbbs/bridge.h"
#include "gbbs/macros.h"
#include "in_memory/clustering/types.h"
#include "in_memory/connected_components/asynchronous_union_find.h"
#include "utils/math.h"

namespace graph_mining {
namespace in_memory {

// This class implements a dendrogram, or a node-weighted tree representing
// a set of clusterings, that can be manipulated in parallel. For example, this
// object is used by the ClusteredGraph object to maintain the dendrogram
// that is generated by a bottom-up hierarchical clustering algorithm.
//
// The dendrogram is constructed by supplying num_nodes, the number of nodes (or
// base objects) that will be clustered. Initially, all nodes are placed in
// their own cluster. As nodes are clustered into each other, the clustering
// algorithm must call MergeToParent below to indicate the newly created
// cluster. Note that the clustering algorithm must assign new cluster-ids, and
// ensure that the newly created clusters are in the range [num_nodes, 2*
// num_nodes - 1).
class ParallelDendrogram {
 public:
  using NodeId = graph_mining::in_memory::NodeId;

  // An edge from a child cluster to a parent cluster with a float similarity.
  struct ParentEdge {
    NodeId parent_id = kInvalidClusterId;
    float merge_similarity = 0;
  };

  explicit ParallelDendrogram(NodeId num_nodes) : num_nodes_(num_nodes) {
    parent_pointers_ = parlay::sequence<ParentEdge>(MaxClusterId());
    ABSL_CHECK_LT(MaxClusterId(), kInvalidClusterId);
  }

  // For a k-ary merge (a merge between k clusters that creates a new cluster)
  // this function should be called k times, e.g., twice for binary merges.
  void MergeToParent(NodeId child, NodeId parent, float similarity) {
    ABSL_CHECK_EQ(parent_pointers_[child].parent_id, kInvalidClusterId);
    parent_pointers_[child] = {parent, similarity};
  }

  ParentEdge GetParent(NodeId node_id) const {
    return parent_pointers_[node_id];
  }

  bool HasValidParent(NodeId node_id) const {
    return parent_pointers_[node_id].parent_id != kInvalidClusterId;
  }

  // Returns a clustering where the dendrogram is cut with the given similarity
  // threshold, linkage (similarity) threshold. Let's view the dendrogram as an
  // edge-weighted tree, where edges go from child clusters to their parent
  // clusters and have weight equal to their merge similarity. Given the
  // threshold T, we preserve all edges with weight >= T.
  //
  // Note that to handle floating-point precision issues, we actually preserve
  // edges that may be slightly smaller than the provided threshold (we will
  // preserve an edge if AlmostEquals(weight, T) is true.
  //
  // Assuming that all leaf-to-root paths have *non-increasing weights*, i.e.,
  // the merges that a cluster participates in over the course of the algorithm
  // can never increase in similarity, the clusters that are returned are
  // guaranteed to be subtrees of the dendrogram.
  //
  // Note that this method still works when leaf-to-root paths are non-monotone,
  // but can result in emitting clusters that are not subtrees of the
  // dendrogram. The GetSubtreeClustering function (below) can be used to handle
  // the case when leaf-to-root paths are non-monotone.
  //
  // The clustering is returned in a dense format and can be converted into the
  // InMemoryClustering format easily using ClusterIdsToClustering.
  parlay::sequence<std::pair<NodeId, NodeId>> GetClustering(
      float linkage_threshold) const {
    // Initial clustering is the identity clustering.

    AsynchronousUnionFind<NodeId> union_find(MaxClusterId());

    // Next, go over all edges. If the parent-edge from this cluster exists and
    // the linkage similarity is above the threshold, add this edge to the
    // union-find structure.
    parlay::parallel_for(0, MaxClusterId(), [&](NodeId i) {
      const auto [parent_id, similarity] = parent_pointers_[i];
      if (parent_id != kInvalidClusterId &&
          (similarity > linkage_threshold ||
           AlmostEquals(similarity, linkage_threshold))) {
        union_find.Unite(i, parent_id);
      }
    });
    // Emit the output clusters by calling find for each base node.
    parlay::sequence<std::pair<NodeId, NodeId>> final_clusters(num_nodes_);
    parlay::parallel_for(0, num_nodes_, [&](NodeId i) {
      final_clusters[i] = {i, union_find.Find(i)};
    });
    return final_clusters;
  }

  // This method is relevant if the dendrogram potentially has non-monotone
  // leaf-to-root paths. Here, given a linkage (similarity) threshold, the
  // algorithm produces a flat clustering where each cluster is guaranteed to
  // be a subtree of the dendrogram. The algorithm works by assigning the
  // cluster of each node to be the last node along its leaf-to-root path
  // which has a merge similarity that is at least the linkage threshold.
  //
  // Similar to GetClustering above, we preserve edges that may be slightly
  // smaller than the provided threshold (we will preserve an edge if
  // AlmostEquals(weight, T) is true.
  //
  // The clustering is returned in a dense format and can be converted into the
  // InMemoryClustering format easily using ClusterIdsToClustering.
  parlay::sequence<std::pair<NodeId, NodeId>> GetSubtreeClustering(
      float linkage_threshold) const {
    // Initial clustering is the identity clustering.
    AsynchronousUnionFind<NodeId> union_find(MaxClusterId());

    // Since the dendrogram may not be binary and nodes may have different merge
    // similarities from each of their children, we first preprocess to compute
    // the maximum similarity of a merge for each cluster. Note that this
    // preprocessing step only looks at the immediate descendants of each node
    // in the tree, and not the entire subtree.
    auto merge_similarities = parlay::sequence<float>(MaxClusterId(), 0);
    parlay::parallel_for(0, MaxClusterId(), [&](size_t i) {
      auto [parent, similarity] = parent_pointers_[i];
      if (parent != kInvalidClusterId) {
        if (similarity > merge_similarities[parent]) {
          gbbs::write_max(&merge_similarities[parent], similarity);
        }
      }
    });

    // Helper function used to unite a path in the dendrogram between a child
    // and its ancestor.
    auto UniteAlongPath = [&](NodeId child, NodeId ancestor) {
      NodeId child_component = union_find.Find(child);
      NodeId ancestor_component = union_find.Find(ancestor);
      // Shortcut: if Find(child) == Find(ancestor) no need to do anything.
      if (child_component != ancestor_component) {
        // Otherwise, perform unite operations along the path from child to the
        // ancestor.
        while (child != ancestor) {
          union_find.Unite(child, ancestor);
          child = parent_pointers_[child].parent_id;
        }
      }
    };

    // For each base node, walk the leaf-to-root path, find the last node in
    // this path with merge similarity >= linkage_threshold and unite along this
    // path. Note that the work of this step can be O(sum of all leaf-to-root
    // path lengths).
    // TODO: use pointer-jumping to ensure near-linear work.
    parlay::parallel_for(0, num_nodes_, [&](size_t i) {
      NodeId child = i;
      NodeId last_root = i;
      while (parent_pointers_[child].parent_id != kInvalidClusterId) {
        // Check if we can prune this search. The condition is if the child
        // and parent are already in the same component.
        NodeId parent = parent_pointers_[child].parent_id;
        float similarity = merge_similarities[parent];
        NodeId child_component = union_find.Find(child);
        NodeId parent_component = union_find.Find(parent);
        // Note that the child and parent can be in the same component, but due
        // to concurrent updates in the union-find structure, child_component
        // and parent_component could be different. However, this does not
        // affect correctness, and can only cause the algorithm to perform
        // some extra work. Note that this benign race does not seem to cause
        // any issues with TSAN currently (tested using --config=tsan).
        if (child_component != parent_component) {
          // We use Math::AlmostEqual below as suggested in go/totw/8 to handle
          // floating point equality issues that can arise during weight
          // computation.
          if (similarity > linkage_threshold ||
              AlmostEquals(similarity, linkage_threshold)) {
            // The merge that created the parent is >= linkage_threshold.
            // Perform unite operations from last_root to the parent and
            // update last_root.
            UniteAlongPath(last_root, parent);
            last_root = parent;
          }
          child = parent;
        } else {
          // Unite along the path from the last_root to the child. Child and
          // parent are already in the same cluster, but we may not have
          // united the path from the last_root to the child.
          UniteAlongPath(last_root, child);
          break;
        }
      }
    });

    // Emit the output clusters by calling find for each base node.
    parlay::sequence<std::pair<NodeId, NodeId>> final_clusters(num_nodes_);
    parlay::parallel_for(0, num_nodes_, [&](NodeId i) {
      final_clusters[i] = {i, union_find.Find(i)};
    });

    return final_clusters;
  }

 private:
  static constexpr size_t kInvalidClusterId =
      std::numeric_limits<NodeId>::max();

  size_t MaxClusterId() const { return 2 * num_nodes_ - 1; }

  parlay::sequence<ParentEdge> parent_pointers_;
  NodeId num_nodes_;
};

}  // namespace in_memory
}  // namespace graph_mining

#endif  // THIRD_PARTY_GRAPH_MINING_IN_MEMORY_CLUSTERING_PARALLEL_DENDROGRAM_H_
