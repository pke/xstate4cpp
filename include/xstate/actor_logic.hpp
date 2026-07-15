#pragma once

#include <any>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "errors.hpp"
#include "event.hpp"

namespace xstate {

enum class SnapshotStatus { Active, Done, Error, Stopped };

class Actor;
using ActorRef = std::shared_ptr<Actor>;
class ActorSystem;

struct Snapshot {
  SnapshotStatus status = SnapshotStatus::Active;
  std::any output;
  std::any error;
  virtual ~Snapshot() = default;

  // Polymorphic copy: the Actor uses this to derive Error/Stopped snapshots
  // and to refresh the children map without knowing the concrete type.
  virtual std::shared_ptr<Snapshot> clone() const { return std::make_shared<Snapshot>(*this); }
  virtual void setChildren(const std::map<std::string, ActorRef>&) {}
  // JSON of the state value for inspection ("null" for non-machine snapshots).
  virtual std::string valueJson() const { return "null"; }
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

struct ActorLogic;

// Deferred side-effect description. Logic appends these to the ActorScope
// during a transition; the Actor executes them in order afterwards. This is
// what keeps MachineLogic::transition pure.
struct Effect {
  enum class Kind { StartTimer, CancelTimer, Send, SpawnChild, StopChild, Emit, Log };

  Kind kind;
  std::string id;       // timer sendId / child id
  std::string target;   // Send/StartTimer destination: "" = self, "__parent__", child id, systemId
  Event event;          // Send/StartTimer/Emit payload
  long long delayMs = 0;
  std::string src;      // SpawnChild logic name (diagnostics)
  std::any input;       // SpawnChild input
  std::string message;  // Log
  bool autoStopOnExit = false;  // SpawnChild from invoke (vs spawnChild action)
  std::any restore;             // SpawnChild: persisted snapshot to restore from
  // SpawnChild: bound by the emitting logic (e.g. the machine's `actors`
  // options), so the Actor never needs behavior-specific knowledge.
  std::function<std::shared_ptr<ActorLogic>(const std::any& input)> factory;
};

struct ActorScope {
  Actor* self = nullptr;         // null in pure MachineLogic tests
  ActorSystem* system = nullptr;
  std::vector<Effect> effects;   // filled during transition; executed by Actor
};

// The universal behavior interface (v5: "actor logic"). Machine, async,
// callback, and transition logics all implement this.
struct ActorLogic {
  virtual ~ActorLogic() = default;

  virtual SnapshotPtr getInitialSnapshot(ActorScope&, const std::any& input) = 0;
  virtual SnapshotPtr transition(SnapshotPtr current, const Event&, ActorScope&) = 0;
  virtual void start(SnapshotPtr, ActorScope&) {}
  virtual void onStop(SnapshotPtr, ActorScope&) {}  // cleanup hook (fromCallback)

  virtual std::any getPersistedSnapshot(SnapshotPtr) const {
    throw ConfigError("this actor logic does not support persistence");
  }
  virtual SnapshotPtr restoreSnapshot(const std::any&, ActorScope&) {
    throw ConfigError("this actor logic does not support persistence");
  }
  virtual std::string persistedToJson(const std::any&) const {
    throw ConfigError("this actor logic does not support JSON persistence");
  }
};

}  // namespace xstate
