#pragma once

#include <any>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "config.hpp"

namespace xstate {
namespace detail {

// Parsed, immutable statechart node. Built once by createMachine; all string
// targets are pre-resolved to node pointers, so transitions never do lookups.
template <typename C>
struct StateNode {
  std::string key;   // segment name ("walk")
  std::string id;    // unique id (explicit or "<machineId>.<path>")
  std::string path;  // dot-joined keys from root ("red.walk"); "" for root
  int docIndex = 0;  // document order (root = 0); exit runs descending, entry ascending

  StateType type = StateType::Atomic;
  HistoryType history = HistoryType::Shallow;

  StateNode* parent = nullptr;
  std::vector<std::unique_ptr<StateNode>> children;  // document order
  StateNode* initial = nullptr;                      // resolved initial child
  StateNode* historyDefault = nullptr;               // resolved history target

  std::vector<ActionRef<C>> entry, exit;
  std::vector<ActionRef<C>> initialActions;  // actions of the initial transition

  struct TransitionDef {
    std::string eventType;             // "" = eventless (always)
    std::vector<StateNode*> targets;   // empty = targetless
    std::optional<GuardRef<C>> guard;
    std::vector<ActionRef<C>> actions;
    bool reenter = false;
    bool forbidden = false;  // `on: { EV: {} }`: blocks bubbling, does nothing
    StateNode* source = nullptr;
  };
  std::vector<TransitionDef> transitions;  // definition order

  struct DelayDef {
    std::string key;    // raw `after` key: decimal ms or delay name
    long long ms = 0;   // valid when !named
    bool named = false;
    std::string eventType;  // "xstate.after(<key>)#<id>"
  };
  std::vector<DelayDef> delays;

  std::vector<InvokeConfig<C>> invokes;  // ids defaulted
  std::vector<std::string> tags;
  std::any output;

  bool isDescendantOf(const StateNode* a) const {
    for (const StateNode* n = this; n != nullptr; n = n->parent)
      if (n == a) return true;
    return false;
  }

  StateNode* childByKey(const std::string& k) const {
    for (const auto& c : children)
      if (c->key == k) return c.get();
    return nullptr;
  }
};

}  // namespace detail
}  // namespace xstate
