#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../event.hpp"

namespace xstate {

// Guard description: named reference, inline predicate, or the v5 algebra
// (and/or/not/stateIn). Evaluated by MachineLogic during transition selection.
template <typename C>
struct GuardRef {
  enum class Kind { Named, Inline, And, Or, Not, StateIn };

  Kind kind = Kind::Named;
  std::string name;                                    // Named / StateIn path
  std::function<bool(const C&, const Event&)> fn;      // Inline
  std::vector<GuardRef> operands;                      // And/Or/Not
};

template <typename C>
GuardRef<C> guardNamed(std::string name) {
  GuardRef<C> g;
  g.kind = GuardRef<C>::Kind::Named;
  g.name = std::move(name);
  return g;
}

template <typename C>
GuardRef<C> guardFn(std::function<bool(const C&, const Event&)> fn) {
  GuardRef<C> g;
  g.kind = GuardRef<C>::Kind::Inline;
  g.fn = std::move(fn);
  return g;
}

template <typename C>
GuardRef<C> and_(std::vector<GuardRef<C>> operands) {
  GuardRef<C> g;
  g.kind = GuardRef<C>::Kind::And;
  g.operands = std::move(operands);
  return g;
}

template <typename C>
GuardRef<C> or_(std::vector<GuardRef<C>> operands) {
  GuardRef<C> g;
  g.kind = GuardRef<C>::Kind::Or;
  g.operands = std::move(operands);
  return g;
}

template <typename C>
GuardRef<C> not_(GuardRef<C> operand) {
  GuardRef<C> g;
  g.kind = GuardRef<C>::Kind::Not;
  g.operands.push_back(std::move(operand));
  return g;
}

template <typename C>
GuardRef<C> not_(std::string named) {
  return not_<C>(guardNamed<C>(std::move(named)));
}

template <typename C>
GuardRef<C> stateIn(std::string path) {
  GuardRef<C> g;
  g.kind = GuardRef<C>::Kind::StateIn;
  g.name = std::move(path);
  return g;
}

}  // namespace xstate
