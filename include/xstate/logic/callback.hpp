#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "../system.hpp"

namespace xstate {

// Handles given to a fromCallback body: sendBack() delivers events to the
// parent (thread-safe); onReceive() registers the handler for events sent TO
// this actor (invoked on the executor thread).
struct CallbackHandle {
  std::function<void(Event)> sendBack;
  std::function<void(std::function<void(const Event&)>)> onReceive;
};

namespace detail {

struct CallbackSnapshot : Snapshot {
  std::any input;
  std::shared_ptr<Snapshot> clone() const override {
    return std::make_shared<CallbackSnapshot>(*this);
  }
};

// One logic instance per actor (actor factories create a fresh one per
// spawn); it holds the registered receive handler and the cleanup function.
class CallbackLogic : public ActorLogic {
 public:
  using Body = std::function<std::function<void()>(CallbackHandle, const std::any& input)>;

  explicit CallbackLogic(Body body) : body_(std::move(body)) {}

  SnapshotPtr getInitialSnapshot(ActorScope&, const std::any& input) override {
    auto snap = std::make_shared<CallbackSnapshot>();
    snap->input = input;
    return snap;
  }

  void start(SnapshotPtr snap, ActorScope& scope) override {
    auto cur = std::static_pointer_cast<const CallbackSnapshot>(snap);
    CallbackHandle handle;
    std::weak_ptr<Actor> self =
        scope.self != nullptr ? scope.self->weak_from_this() : std::weak_ptr<Actor>{};
    handle.sendBack = [self](Event e) {
      if (auto s = self.lock())
        if (auto p = s->parent().lock()) p->send(std::move(e));
    };
    handle.onReceive = [this](std::function<void(const Event&)> handler) {
      receive_ = std::move(handler);
    };
    cleanup_ = body_(std::move(handle), cur->input);
  }

  SnapshotPtr transition(SnapshotPtr current, const Event& event, ActorScope&) override {
    if (receive_) receive_(event);
    return current;  // callback actors have no state transitions of their own
  }

  void onStop(SnapshotPtr, ActorScope&) override {
    if (cleanup_) {
      auto fn = std::move(cleanup_);
      cleanup_ = nullptr;
      fn();
    }
  }

 private:
  Body body_;
  std::function<void(const Event&)> receive_;
  std::function<void()> cleanup_;
};

}  // namespace detail

inline std::shared_ptr<ActorLogic> fromCallback(detail::CallbackLogic::Body body) {
  return std::make_shared<detail::CallbackLogic>(std::move(body));
}

}  // namespace xstate
