#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "../system.hpp"

namespace xstate {

// fromAsync (== v5 fromPromise): wraps one-shot asynchronous work. The body
// receives a Resolver whose resolve()/reject() may be called from ANY thread —
// each just enqueues an event through the actor's thread-safe send(). The
// first outcome wins; later calls hit a non-Active actor and are dropped.
class Resolver {
 public:
  Resolver() = default;
  explicit Resolver(std::weak_ptr<Actor> actor) : actor_(std::move(actor)) {}

  void resolve(std::any output) const {
    if (auto a = actor_.lock()) a->send(Event{"xstate.promise.resolve", std::move(output)});
  }
  void reject(std::any error) const {
    if (auto a = actor_.lock()) a->send(Event{"xstate.promise.reject", std::move(error)});
  }

 private:
  std::weak_ptr<Actor> actor_;
};

namespace detail {

struct AsyncSnapshot : Snapshot {
  std::any input;
  std::shared_ptr<Snapshot> clone() const override {
    return std::make_shared<AsyncSnapshot>(*this);
  }
};

class AsyncLogic : public ActorLogic {
 public:
  explicit AsyncLogic(std::function<void(Resolver, const std::any&)> body)
      : body_(std::move(body)) {}

  SnapshotPtr getInitialSnapshot(ActorScope&, const std::any& input) override {
    auto snap = std::make_shared<AsyncSnapshot>();
    snap->input = input;
    return snap;
  }

  void start(SnapshotPtr snap, ActorScope& scope) override {
    auto cur = std::static_pointer_cast<const AsyncSnapshot>(snap);
    Resolver resolver(scope.self != nullptr ? scope.self->weak_from_this()
                                            : std::weak_ptr<Actor>{});
    body_(std::move(resolver), cur->input);
  }

  SnapshotPtr transition(SnapshotPtr current, const Event& event, ActorScope&) override {
    if (event.type == "xstate.promise.resolve") {
      auto next = current->clone();
      next->status = SnapshotStatus::Done;
      next->output = event.data;
      return next;
    }
    if (event.type == "xstate.promise.reject") {
      auto next = current->clone();
      next->status = SnapshotStatus::Error;
      next->error = event.data;
      return next;
    }
    return current;  // async work ignores other events
  }

 private:
  std::function<void(Resolver, const std::any&)> body_;
};

}  // namespace detail

inline std::shared_ptr<ActorLogic> fromAsync(
    std::function<void(Resolver, const std::any& input)> body) {
  return std::make_shared<detail::AsyncLogic>(std::move(body));
}

}  // namespace xstate
