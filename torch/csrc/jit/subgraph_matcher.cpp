#include <torch/csrc/jit/subgraph_matcher.h>
#include <stack>

namespace torch {
namespace jit {
namespace {

/**
 * \brief A class implementing an API for comparing subgraphs.
 */
class SubgraphMatcher {
 public:
  SubgraphMatcher(const Graph& pattern, const Graph& graph)
      : pattern_(pattern), graph_(graph) {}

  /**
   * \brief Compare matchGraph with the part of the graph denoted by a node \p
   * ANCHOR.
   *
   * The anchor node would be compared against the deepest node in the
   * match-graph. A node is considered matching if its number of inputs/outputs
   * is the same as in the corresponding matchGraph node, its type is the same,
   * and all nodes producing input-values also match.
   */
  bool matchesSubgraphFromAnchorNode(const Node* anchor);

  /** \brief Return match map for nodes. */
  std::unordered_map<const Node*, const Node*> nodes_map() const {
    return nodes_map_;
  }

  /** \brief Return match map for values. */
  std::unordered_map<const Value*, const Value*> values_map() const {
    return values_map_;
  }

 private:
  bool matchValues(const Value* v1, const Value* v2);
  bool matchNodes(const Node* n1, const Node* n2);

  std::unordered_map<const Node*, const Node*> nodes_map_;
  std::unordered_map<const Value*, const Value*> values_map_;

  const Graph& pattern_;
  const Graph& graph_;
  const Node* anchor_;
};

/**
 * \brief A function to verify that \p PATTERN is valid. Concrete requirements
 * for validity can be found in subgraph_matcher.h.
 */
bool patternGraphIsValid(const Graph& pattern) {
  // Verify that pattern graph has a single block.
  for (const Node* n : pattern.nodes()) {
    if (!n->blocks().empty()) {
      return false;
    }
  }

  // Verify that pattern graph returns only one value.
  const Node* bottom_node = *(pattern.nodes().end());
  if (bottom_node->inputs().size() != 1) {
    return false;
  }

  // TODO: Verify that nodes in the pattern don't alias.
  return true;
}

/**
 * Compare two Values. V1 is from pattern, V2 is from the actual graph.
 *
 * The values are considered matching if:
 * 1) the nodes defining them match
 * 2) they have the same number of uses, except they are entry or exit nodes.
 */
bool SubgraphMatcher::matchValues(const Value* v1, const Value* v2) {
  // Check if we've already visited these values.
  if (values_map_.count(v1)) {
    return values_map_.at(v1) == v2;
  }

  // When V2 is ANCHOR, we're comparing exiting values, and when V1->node is
  // PARAM, we're comparing entering values - in these two cases the number of
  // uses don't need to be the same.
  if (v1->uses().size() != v2->uses().size() && v2->node() != anchor_ &&
      v1->node()->kind() != prim::Param) {
    return false;
  }

  // Add the values to the map before calling matchNodes to avoid infinite
  // recursion.
  values_map_[v1] = v2;
  return matchNodes(v1->node(), v2->node());
}

/**
 * Compare two Nodes. N1 is from pattern, N2 is from the actual graph.
 *
 * The nodes are considered matching if:
 * 1) N1 and N2 are of the same kind.
 * 2) Number of inputs and outputs is the same.
 * 3) All input and output values match.
 *
 * A special case is when N1 is PARAM - this is considered outside the pattern,
 * so it matches everything.
 */
bool SubgraphMatcher::matchNodes(const Node* n1, const Node* n2) {
  // Check if we've already visited these nodes.
  if (nodes_map_.count(n1)) {
    return nodes_map_.at(n1) == n2;
  }

  // Param node in pattern graph matches everything.
  if (n1->kind() == prim::Param) {
    return true;
  }

  // We don't allow matches to span across blocks, so check if N2 is in the same
  // block as the first (anchor) node.
  if (n2->owningBlock() != anchor_->owningBlock()) {
    return false;
  }

  if (n1->kind() != n2->kind() ||
      n1->outputs().size() != n2->outputs().size() ||
      n1->inputs().size() != n2->inputs().size()) {
    return false;
  }

  // Add nodes to the map before calling matchValues to avoid infinite
  // recursion.
  nodes_map_[n1] = n2;
  for (size_t i = 0; i < n1->outputs().size(); i++) {
    if (!matchValues(n1->outputs()[i], n2->outputs()[i])) {
      return false;
    }
  }
  for (size_t i = 0; i < n1->inputs().size(); i++) {
    if (!matchValues(n1->inputs()[i], n2->inputs()[i])) {
      return false;
    }
  }

  return true;
}

/**
 * Recursively try to match pattern with the actual graph starting from the
 * exiting node in the pattern and anchor node in the actual graph.
 */
bool SubgraphMatcher::matchesSubgraphFromAnchorNode(const Node* anchor) {
  nodes_map_.clear();
  values_map_.clear();
  anchor_ = anchor;

  const Node* bottom_node = *(pattern_.nodes().end());
  AT_ASSERT(bottom_node->inputs().size() == 1);
  bottom_node = bottom_node->input()->node();

  if (!matchNodes(bottom_node, anchor)) {
    return false;
  }

  return true;
}

} // unnamed namespace

// Main entry point for the subgraph matching.
std::vector<Match> findPatternMatches(
    const Graph& pattern,
    const Graph& graph) {
  AT_ASSERT(patternGraphIsValid(pattern));

  SubgraphMatcher m(pattern, graph);
  std::vector<Match> matches;
  std::stack<const Block*> blocks_to_visit;

  // Iterate over all nodes in the graph (including nodes in subblocks) trying
  // to match the pattern each node.
  blocks_to_visit.push(graph.block());
  while (!blocks_to_visit.empty()) {
    const Block* block = blocks_to_visit.top();
    blocks_to_visit.pop();
    for (const Node* n : block->nodes()) {
      if (m.matchesSubgraphFromAnchorNode(n)) {
        matches.push_back({n, m.nodes_map(), m.values_map()});
      }
      for (const Block* subblock : n->blocks()) {
        blocks_to_visit.push(subblock);
      }
    }
  }
  return matches;
}

} // namespace jit
} // namespace torch
