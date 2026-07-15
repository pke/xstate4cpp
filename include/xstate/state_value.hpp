#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace xstate {

// Recursive state value: a leaf string ("idle") or a map of region/child
// values ({auth: {card: "waiting"}}). Mirrors xstate's StateValue.
class StateValue {
 public:
  StateValue() = default;
  StateValue(std::string leaf) : leaf_(std::move(leaf)) {}
  StateValue(const char* leaf) : leaf_(leaf) {}

  static StateValue fromPath(std::string_view dotted) {
    auto dot = dotted.find('.');
    if (dot == std::string_view::npos) return StateValue{std::string(dotted)};
    StateValue v;
    v.branches_.emplace(std::string(dotted.substr(0, dot)),
                        fromPath(dotted.substr(dot + 1)));
    return v;
  }

  static StateValue branchesOf(std::map<std::string, StateValue> m) {
    StateValue v;
    v.branches_ = std::move(m);
    return v;
  }

  bool empty() const { return leaf_.empty() && branches_.empty(); }
  bool isLeaf() const { return branches_.empty() && !leaf_.empty(); }
  const std::string& leaf() const { return leaf_; }
  const std::map<std::string, StateValue>& branches() const { return branches_; }

  // xstate matches() semantics: `other` is a (possibly partial) path/value
  // that must be covered by this value.
  bool contains(const StateValue& other) const {
    if (other.empty()) return true;
    if (isLeaf()) return other.isLeaf() && other.leaf_ == leaf_;
    if (other.isLeaf()) return branches_.count(other.leaf_) > 0;  // partial path prefix
    for (const auto& [key, sub] : other.branches_) {
      auto it = branches_.find(key);
      if (it == branches_.end() || !it->second.contains(sub)) return false;
    }
    return true;
  }

  std::string toString() const {
    if (isLeaf()) return leaf_;
    std::string out = "{";
    bool first = true;
    for (const auto& [key, sub] : branches_) {
      if (!first) out += ", ";
      first = false;
      out += key + ": " + sub.toString();
    }
    return out + "}";
  }

  bool operator==(const StateValue& o) const {
    return leaf_ == o.leaf_ && branches_ == o.branches_;
  }
  bool operator!=(const StateValue& o) const { return !(*this == o); }

 private:
  std::string leaf_;
  std::map<std::string, StateValue> branches_;
};

}  // namespace xstate
