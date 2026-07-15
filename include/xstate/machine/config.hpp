#pragma once

#include <any>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../errors.hpp"
#include "../event.hpp"
#include "actions.hpp"
#include "guards.hpp"

namespace xstate {

struct ActorLogic;  // actor_logic.hpp

// Insertion-ordered map: statechart siblings follow document order (matters
// for parallel-region entry/exit ordering and transition conflict resolution),
// which std::map's alphabetical iteration would break. Linear find is fine at
// config scale.
template <typename V>
class OrderedMap {
 public:
  using value_type = std::pair<std::string, V>;
  using iterator = typename std::vector<value_type>::iterator;
  using const_iterator = typename std::vector<value_type>::const_iterator;

  V& operator[](const std::string& key) {
    for (auto& item : items_)
      if (item.first == key) return item.second;
    items_.emplace_back(key, V{});
    return items_.back().second;
  }
  const V* find(const std::string& key) const {
    for (const auto& item : items_)
      if (item.first == key) return &item.second;
    return nullptr;
  }
  std::size_t count(const std::string& key) const { return find(key) ? 1 : 0; }
  std::size_t size() const { return items_.size(); }
  bool empty() const { return items_.empty(); }
  iterator begin() { return items_.begin(); }
  iterator end() { return items_.end(); }
  const_iterator begin() const { return items_.begin(); }
  const_iterator end() const { return items_.end(); }

 private:
  std::vector<value_type> items_;
};

enum class StateType { Atomic, Compound, Parallel, Final, History };
enum class HistoryType { Shallow, Deep };

template <typename C>
struct TransitionConfig {
  std::vector<std::string> targets;  // empty = targetless
  std::optional<GuardRef<C>> guard;
  std::vector<ActionRef<C>> actions;
  bool reenter = false;
  std::string description;

  TransitionConfig() = default;
  TransitionConfig(const char* t) : targets{t} {}
  TransitionConfig(std::string t) : targets{std::move(t)} {}

  TransitionConfig& target(std::string t) {
    targets.push_back(std::move(t));
    return *this;
  }
  TransitionConfig& guarded(GuardRef<C> g) {
    guard = std::move(g);
    return *this;
  }
  TransitionConfig& guarded(std::string named) {
    guard = guardNamed<C>(std::move(named));
    return *this;
  }
  TransitionConfig& act(ActionRef<C> a) {
    actions.push_back(std::move(a));
    return *this;
  }
  TransitionConfig& act(std::string named) {
    actions.push_back(action<C>(std::move(named)));
    return *this;
  }
};

template <typename C>
TransitionConfig<C> transition(std::string target = "") {
  TransitionConfig<C> t;
  if (!target.empty()) t.targets.push_back(std::move(target));
  return t;
}

// Candidate transitions for one event, in definition order.
template <typename C>
struct TransitionList {
  std::vector<TransitionConfig<C>> list;

  TransitionList& operator=(const char* target) {
    list.assign(1, TransitionConfig<C>{target});
    return *this;
  }
  TransitionList& operator=(std::string target) {
    list.assign(1, TransitionConfig<C>{std::move(target)});
    return *this;
  }
  TransitionList& operator=(TransitionConfig<C> t) {
    list.assign(1, std::move(t));
    return *this;
  }
  TransitionList& add(TransitionConfig<C> t) {
    list.push_back(std::move(t));
    return *this;
  }
};

// `after` map supporting both after[10000] and after["NAMED_DELAY"].
// Numeric keys are stored as decimal strings.
template <typename C>
struct AfterMap {
  std::map<std::string, TransitionList<C>> entries;

  TransitionList<C>& operator[](long long ms) { return entries[std::to_string(ms)]; }
  TransitionList<C>& operator[](const std::string& delayName) { return entries[delayName]; }
  TransitionList<C>& operator[](const char* delayName) { return entries[delayName]; }
  bool empty() const { return entries.empty(); }
};

template <typename C>
struct InvokeConfig {
  std::string src;
  std::string id;  // defaults to src at parse
  std::any input;
  std::vector<TransitionConfig<C>> onDone, onError;
};

template <typename C>
struct InvokeBuilder {
  InvokeConfig<C> cfg;

  explicit InvokeBuilder(std::string src) { cfg.src = std::move(src); }

  InvokeBuilder& id(std::string i) {
    cfg.id = std::move(i);
    return *this;
  }
  InvokeBuilder& input(std::any in) {
    cfg.input = std::move(in);
    return *this;
  }
  InvokeBuilder& onDone(TransitionConfig<C> t) {
    cfg.onDone.push_back(std::move(t));
    return *this;
  }
  InvokeBuilder& onDone(std::string target, ActionRef<C> a) {
    cfg.onDone.push_back(TransitionConfig<C>{std::move(target)}.act(std::move(a)));
    return *this;
  }
  InvokeBuilder& onError(TransitionConfig<C> t) {
    cfg.onError.push_back(std::move(t));
    return *this;
  }

  operator InvokeConfig<C>() const { return cfg; }
};

template <typename C>
InvokeBuilder<C> invoke(std::string src) {
  return InvokeBuilder<C>{std::move(src)};
}

template <typename C>
struct StateConfig {
  StateType type = StateType::Atomic;  // auto-Compound at parse if states non-empty
  HistoryType history = HistoryType::Shallow;
  std::string id;       // for "#id" targets
  std::string initial;
  std::vector<ActionRef<C>> initialActions;  // run when the initial transition is taken
  std::string target;   // history default target
  std::vector<ActionRef<C>> entry, exit;
  OrderedMap<TransitionList<C>> on;  // "*" wildcard supported
  AfterMap<C> after;
  std::vector<TransitionConfig<C>> always;
  std::vector<TransitionConfig<C>> onDone;  // fires on "xstate.done.state.<id>"
  std::vector<InvokeConfig<C>> invoke;
  OrderedMap<StateConfig> states;
  std::vector<std::string> tags;
  std::any output;  // final-state output (value or fn)
};

template <typename C>
struct MachineConfig {
  std::string id = "(machine)";
  StateType type = StateType::Compound;  // Compound or Parallel roots only
  std::string initial;
  std::vector<ActionRef<C>> initialActions;  // run when the root initial transition is taken
  C context{};
  OrderedMap<StateConfig<C>> states;
  std::vector<ActionRef<C>> entry, exit;
  OrderedMap<TransitionList<C>> on;
  std::vector<TransitionConfig<C>> always;
  std::any output;
};

template <typename C>
struct MachineOptions {
  std::map<std::string, std::function<void(C&, const Event&)>> actions;
  std::map<std::string, std::function<bool(const C&, const Event&)>> guards;
  std::map<std::string,
           std::variant<long long, std::function<long long(const C&, const Event&)>>>
      delays;
  std::map<std::string, std::function<std::shared_ptr<ActorLogic>(const std::any& input)>>
      actors;
};

}  // namespace xstate
