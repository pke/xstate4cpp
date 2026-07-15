#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "actor_logic.hpp"
#include "interfaces/clock.hpp"
#include "interfaces/executor.hpp"

namespace xstate {

class ActorSystem;

struct ActorOptions {
  std::any input;
  std::any snapshot;     // persisted snapshot to restore (see persistence)
  std::string systemId;  // receptionist registration; empty = unregistered
};

// Runtime shell around any ActorLogic: lifecycle, serialized event
// processing via the system's Executor, observers, timers, children.
//
// Threading model: send() and getSnapshot() are safe from any thread
// (send only posts; the snapshot pointer is swapped atomically and
// snapshots are immutable). Everything else — all logic, effects,
// observer callbacks — runs serialized on the system executor.
class Actor : public std::enable_shared_from_this<Actor> {
 public:
  void start();            // idempotent; processing begins on the executor
  void send(Event event);  // thread-safe; dropped unless actor is Active
  void stop();             // stops children first; observers see Stopped

  SnapshotPtr getSnapshot() const { return std::atomic_load(&snapshot_); }

  std::uint64_t subscribe(std::function<void(SnapshotPtr)> observer) {
    observers_.emplace(nextSubId_, std::move(observer));
    return nextSubId_++;
  }
  void unsubscribe(std::uint64_t id) { observers_.erase(id); }

  // Handler for events emitted via the emit() action.
  std::uint64_t on(std::string emittedType, std::function<void(const Event&)> handler) {
    emitHandlers_.emplace(nextSubId_, std::make_pair(std::move(emittedType), std::move(handler)));
    return nextSubId_++;
  }

  const std::string& id() const { return id_; }
  const std::string& sessionId() const { return sessionId_; }
  std::weak_ptr<Actor> parent() const { return parent_; }

  // Persistence. Call on the executor thread or while the system is
  // quiescent — the children/timers bookkeeping is not lock-protected.
  inline std::any getPersistedSnapshot() const;
  std::string getPersistedSnapshotJson() const {
    return logic_->persistedToJson(getPersistedSnapshot());
  }

 private:
  friend class ActorSystem;
  Actor() = default;

  // Defined in system.hpp (need the complete ActorSystem):
  inline void inspect(struct InspectionEvent ev) const;
  inline void inspectSnapshotChanged() const;
  inline void doStart();
  inline void doReceive(const Event& event);
  inline void doStop();
  inline void executeEffects(ActorScope& scope);
  inline void executeEffect(Effect& effect);
  inline ActorRef resolveTarget(const std::string& target);
  inline void becomeError(const std::string& message);
  inline void syncChildrenIntoSnapshot();
  inline void notifyObservers();
  inline void reportCompletionToParent();
  inline ActorScope makeScope();

  void storeSnapshot(SnapshotPtr snap) { std::atomic_store(&snapshot_, std::move(snap)); }

  std::shared_ptr<ActorLogic> logic_;
  SnapshotPtr snapshot_;  // accessed via atomic_load/atomic_store only
  // The Executor/Clock are owned by the caller and must outlive every actor
  // (native adapters guarantee this: their destructors drain and join before
  // returning). The ActorSystem may die earlier — e.g. queued stop tasks
  // running after the user dropped the system — hence only a weak ref.
  Executor* executor_ = nullptr;
  Clock* clock_ = nullptr;
  std::weak_ptr<ActorSystem> system_;
  std::weak_ptr<Actor> parent_;
  std::string id_;
  std::string sessionId_;
  std::any input_;
  std::any restoreFrom_;
  bool startRequested_ = false;
  bool completionReported_ = false;

  std::map<std::uint64_t, std::function<void(SnapshotPtr)>> observers_;
  std::map<std::uint64_t, std::pair<std::string, std::function<void(const Event&)>>> emitHandlers_;
  std::uint64_t nextSubId_ = 1;

  std::map<std::string, TimerId> timers_;  // sendId -> platform timer
  std::map<std::string, ActorRef> children_;
  std::vector<std::string> autoStopChildren_;  // invoke-spawned: stopped on state exit

  struct ChildMeta {
    std::string src;
    std::any input;
  };
  std::map<std::string, ChildMeta> childMeta_;  // for persistence/respawn
};

}  // namespace xstate
