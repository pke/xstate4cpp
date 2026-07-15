#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "../actor_logic.hpp"

namespace xstate {

// fromTransition: a plain reducer (State, Event) -> State.
template <typename S>
struct TransitionSnapshot : Snapshot {
  S state{};
  std::shared_ptr<Snapshot> clone() const override {
    return std::make_shared<TransitionSnapshot>(*this);
  }
};

namespace detail {

template <typename S>
class TransitionLogic : public ActorLogic {
 public:
  TransitionLogic(std::function<S(S, const Event&)> reducer, S initial)
      : reducer_(std::move(reducer)), initial_(std::move(initial)) {}

  SnapshotPtr getInitialSnapshot(ActorScope&, const std::any&) override {
    auto snap = std::make_shared<TransitionSnapshot<S>>();
    snap->state = initial_;
    return snap;
  }

  SnapshotPtr transition(SnapshotPtr current, const Event& event, ActorScope&) override {
    auto cur = std::static_pointer_cast<const TransitionSnapshot<S>>(current);
    auto next = std::make_shared<TransitionSnapshot<S>>();
    next->state = reducer_(cur->state, event);
    return next;
  }

 private:
  std::function<S(S, const Event&)> reducer_;
  S initial_;
};

}  // namespace detail

template <typename S>
std::shared_ptr<ActorLogic> fromTransition(std::function<S(S, const Event&)> reducer,
                                           S initial) {
  return std::make_shared<detail::TransitionLogic<S>>(std::move(reducer), std::move(initial));
}

template <typename S>
const S* transitionState(const SnapshotPtr& p) {
  auto snap = std::dynamic_pointer_cast<const TransitionSnapshot<S>>(p);
  return snap ? &snap->state : nullptr;
}

}  // namespace xstate
