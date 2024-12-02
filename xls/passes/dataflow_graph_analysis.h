// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef XLS_PASSES_DATAFLOW_GRAPH_ANALYSIS_H_
#define XLS_PASSES_DATAFLOW_GRAPH_ANALYSIS_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/ir/node.h"
#include "xls/passes/query_engine.h"
#include "ortools/graph/ebert_graph.h"
#include "ortools/graph/max_flow.h"

namespace xls {

// A class for dataflow graph analysis of the IR instructions in a function,
// ignoring all literals and accounting for all potential external sources of
// data.
//
// For each node, this can find a minimum set of unknown bits (specified in
// terms of the nodes emitting them) that fully determine the value of the node.
class DataflowGraphAnalysis {
 public:
  DataflowGraphAnalysis(FunctionBase* f,
                        const QueryEngine* query_engine = nullptr);

  absl::StatusOr<std::vector<Node*>> GetMinCutFor(
      Node* node, std::optional<int64_t> max_unknown_bits = std::nullopt,
      int64_t* unknown_bits = nullptr);

  absl::StatusOr<int64_t> GetUnknownBitsFor(Node* node);

 private:
  Node* current_sink_ = nullptr;
  absl::Status SolveFor(Node* node);

  static constexpr operations_research::NodeIndex kSourceIndex = 0;
  static constexpr operations_research::NodeIndex kSinkIndex = 1;

  static operations_research::NodeIndex InIndex(size_t topo_index) {
    return static_cast<operations_research::NodeIndex>(2 * topo_index + 2);
  }
  operations_research::NodeIndex InIndex(Node* node) {
    return InIndex(node_to_index_[node]);
  }

  bool IsOutIndex(operations_research::NodeIndex index) {
    return index > 1 && (index % 2) == 1;
  }
  static operations_research::NodeIndex OutIndex(size_t topo_index) {
    return static_cast<operations_research::NodeIndex>(2 * topo_index + 3);
  }
  operations_research::NodeIndex OutIndex(Node* node) {
    return OutIndex(node_to_index_[node]);
  }

  static size_t TopoIndex(operations_research::NodeIndex index) {
    return (index - 2) >> 1;
  }
  size_t TopoIndex(Node* node) { return node_to_index_[node]; }

  using Graph = ::util::ReverseArcStaticGraph<operations_research::NodeIndex,
                                              operations_research::ArcIndex>;

  const std::vector<Node*> nodes_;
  absl::flat_hash_map<Node*, size_t> node_to_index_;

  std::unique_ptr<Graph> graph_;
  absl::flat_hash_map<operations_research::ArcIndex, int64_t> arc_capacities_;

  absl::flat_hash_map<Node*, operations_research::ArcIndex> internal_arcs_;
  absl::flat_hash_map<Node*, operations_research::ArcIndex> source_arcs_;
  absl::flat_hash_map<Node*, operations_research::ArcIndex> sink_arcs_;

  std::unique_ptr<operations_research::GenericMaxFlow<Graph>> max_flow_;

  bool first_ = true;
};

}  // namespace xls

#endif  // XLS_PASSES_DATAFLOW_GRAPH_ANALYSIS_H_