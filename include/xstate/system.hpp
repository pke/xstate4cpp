#pragma once

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "actor.hpp"
#include "detail/json_writer.hpp"
#include "snapshot.hpp"

namespace xstate {

struct SystemOptions {
  Executor* executor = nullptr;  // required
  Clock* clock = nullptr;        // required
};

// Inspection event, serializable to xstate's inspection wire format
// ("@xstate.actor" / "@xstate.event" / "@xstate.snapshot"), so SDK field
// logs can be replayed against Stately tooling. Event payloads (std::any)
// are not serialized — only event types.
struct InspectionEvent {
  enum class Kind { ActorCreated, EventReceived, SnapshotChanged, MicrostepDone };

  Kind kind = Kind::ActorCreated;
  std::string sessionId;
  std::string actorId;
  Event event;           // EventReceived
  SnapshotPtr snapshot;  // SnapshotChanged
  bool deadLetter = false;

  std::string toJson() const {
    std::string out = "{\"type\":";
    switch (kind) {
      case Kind::ActorCreated:
        out += "\"@xstate.actor\",\"sessionId\":" + detail::jsonString(sessionId);
        break;
      case Kind::EventReceived:
        out += "\"@xstate.event\",\"sessionId\":" + detail::jsonString(sessionId) +
               ",\"event\":{\"type\":" + detail::jsonString(event.type) + "}";
        if (deadLetter) out += ",\"deadLetter\":true";
        break;
      case Kind::SnapshotChanged: {
        const char* status = "active";
        if (snapshot) {
          switch (snapshot->status) {
            case SnapshotStatus::Active: status = "active"; break;
            case SnapshotStatus::Done: status = "done"; break;
            case SnapshotStatus::Error: status = "error"; break;
            case SnapshotStatus::Stopped: status = "stopped"; break;
          }
        }
        out += "\"@xstate.snapshot\",\"sessionId\":" + detail::jsonString(sessionId) +
               ",\"snapshot\":{\"status\":\"" + status +
               "\",\"value\":" + (snapshot ? snapshot->valueJson() : "null") + "}";
        break;
      }
      case Kind::MicrostepDone:
        out += "\"@xstate.microstep\",\"sessionId\":" + detail::jsonString(sessionId);
        break;
    }
    return out + "}";
  }
};

// Owns the scheduling seam, the actor tree root, and the receptionist
// registry. All actors of one system share one Executor (serialized) and
// one Clock.
class ActorSystem : public std::enable_shared_from_this<ActorSystem> {
 public:
  static std::shared_ptr<ActorSystem> create(std::shared_ptr<ActorLogic> rootLogic,
                                             SystemOptions options, ActorOptions actorOptions = {}) {
    if (options.executor == nullptr || options.clock == nullptr)
      throw ConfigError("ActorSystem requires both an executor and a clock");
    auto sys = std::shared_ptr<ActorSystem>(new ActorSystem());
    sys->executor_ = options.executor;
    sys->clock_ = options.clock;
    sys->root_ = sys->createActor(std::move(rootLogic), "root", {}, std::move(actorOptions));
    return sys;
  }

  ActorRef root() const { return root_; }

  // Receptionist: only actors registered with an explicit systemId.
  ActorRef get(const std::string& systemId) const {
    auto it = registry_.find(systemId);
    return it == registry_.end() ? nullptr : it->second;
  }

  Executor& executor() const { return *executor_; }
  Clock& clock() const { return *clock_; }

  void stop() {
    if (root_) root_->stop();
  }

  std::uint64_t inspect(std::function<void(const InspectionEvent&)> handler) {
    inspectors_.emplace(nextInspectId_, std::move(handler));
    return nextInspectId_++;
  }
  void unsubscribeInspect(std::uint64_t id) { inspectors_.erase(id); }

 private:
  friend class Actor;
  ActorSystem() = default;

  void emitInspection(const InspectionEvent& ev) const {
    for (const auto& [id, fn] : inspectors_) fn(ev);
  }

  ActorRef createActor(std::shared_ptr<ActorLogic> logic, std::string id,
                       std::weak_ptr<Actor> parent, ActorOptions opts = {}) {
    auto actor = ActorRef(new Actor());
    actor->logic_ = std::move(logic);
    actor->executor_ = executor_;
    actor->clock_ = clock_;
    actor->system_ = weak_from_this();
    actor->parent_ = std::move(parent);
    actor->id_ = std::move(id);
    actor->sessionId_ = "x:" + std::to_string(sessionCounter_++);
    actor->input_ = std::move(opts.input);
    actor->restoreFrom_ = std::move(opts.snapshot);
    if (!opts.systemId.empty()) registry_[opts.systemId] = actor;
    return actor;
  }

  Executor* executor_ = nullptr;
  Clock* clock_ = nullptr;
  ActorRef root_;
  std::map<std::string, ActorRef> registry_;
  std::uint64_t sessionCounter_ = 0;
  std::map<std::uint64_t, std::function<void(const InspectionEvent&)>> inspectors_;
  std::uint64_t nextInspectId_ = 1;
};

template <typename C>
class Machine;

template <typename C>
std::shared_ptr<ActorSystem> createActorSystem(std::shared_ptr<Machine<C>> machine,
                                               SystemOptions options, ActorOptions actorOptions = {}) {
  return ActorSystem::create(std::static_pointer_cast<ActorLogic>(machine), options,
                             std::move(actorOptions));
}

// ---- Actor methods that need the complete ActorSystem ----

inline ActorScope Actor::makeScope() {
  ActorScope scope;
  scope.self = this;
  scope.system = system_.lock().get();  // may be null after system teardown
  return scope;
}

inline void Actor::start() {
  if (startRequested_) return;
  startRequested_ = true;
  auto self = shared_from_this();
  executor_->post([self] { self->doStart(); });
}

inline void Actor::send(Event event) {
  auto self = shared_from_this();
  executor_->post([self, event = std::move(event)] { self->doReceive(event); });
}

inline void Actor::stop() {
  auto self = shared_from_this();
  executor_->post([self] { self->doStop(); });
}

inline void Actor::inspect(InspectionEvent ev) const {
  auto sys = system_.lock();
  if (sys == nullptr) return;  // system already gone: nobody is listening
  ev.sessionId = sessionId_;
  ev.actorId = id_;
  sys->emitInspection(ev);
}

inline void Actor::doStart() {
  if (getSnapshot() != nullptr) return;  // already started
  {
    InspectionEvent created;
    created.kind = InspectionEvent::Kind::ActorCreated;
    inspect(std::move(created));
    InspectionEvent received;
    received.kind = InspectionEvent::Kind::EventReceived;
    received.event = events::init();
    inspect(std::move(received));
  }
  try {
    ActorScope scope = makeScope();
    SnapshotPtr snap = restoreFrom_.has_value()
                           ? logic_->restoreSnapshot(restoreFrom_, scope)
                           : logic_->getInitialSnapshot(scope, input_);
    storeSnapshot(snap);
    logic_->start(snap, scope);
    executeEffects(scope);
    syncChildrenIntoSnapshot();
    inspectSnapshotChanged();
    reportCompletionToParent();
    notifyObservers();
  } catch (const std::exception& e) {
    becomeError(e.what());
  } catch (...) {
    becomeError("unknown error");
  }
}

inline void Actor::inspectSnapshotChanged() const {
  InspectionEvent ev;
  ev.kind = InspectionEvent::Kind::SnapshotChanged;
  ev.snapshot = getSnapshot();
  inspect(std::move(ev));
}

inline void Actor::doReceive(const Event& event) {
  SnapshotPtr cur = getSnapshot();
  {
    InspectionEvent received;
    received.kind = InspectionEvent::Kind::EventReceived;
    received.event = event;
    received.deadLetter = cur == nullptr || cur->status != SnapshotStatus::Active;
    inspect(std::move(received));
  }
  if (cur == nullptr || cur->status != SnapshotStatus::Active) return;  // dead letter
  try {
    ActorScope scope = makeScope();
    SnapshotPtr next = logic_->transition(cur, event, scope);
    const bool changed = next != cur;
    storeSnapshot(next);
    executeEffects(scope);
    syncChildrenIntoSnapshot();
    if (changed) inspectSnapshotChanged();
    reportCompletionToParent();
    if (changed) notifyObservers();
  } catch (const std::exception& e) {
    becomeError(e.what());
  } catch (...) {
    becomeError("unknown error");
  }
}

inline void Actor::doStop() {
  SnapshotPtr cur = getSnapshot();
  if (cur != nullptr && cur->status == SnapshotStatus::Stopped) return;
  // children first (all actors share the executor thread: direct call is safe)
  for (auto& [cid, child] : children_) child->doStop();
  children_.clear();
  for (auto& [sid, timer] : timers_) clock_->clearTimeout(timer);
  timers_.clear();
  try {
    ActorScope scope = makeScope();
    logic_->onStop(cur, scope);
    executeEffects(scope);
  } catch (...) {
    // cleanup errors are swallowed: the actor is stopping regardless
  }
  std::shared_ptr<Snapshot> stopped = cur ? cur->clone() : std::make_shared<Snapshot>();
  stopped->status = SnapshotStatus::Stopped;
  storeSnapshot(std::move(stopped));
  inspectSnapshotChanged();
  notifyObservers();
}

inline void Actor::becomeError(const std::string& message) {
  SnapshotPtr cur = getSnapshot();
  std::shared_ptr<Snapshot> err = cur ? cur->clone() : std::make_shared<Snapshot>();
  err->status = SnapshotStatus::Error;
  err->error = message;
  storeSnapshot(std::move(err));
  inspectSnapshotChanged();
  reportCompletionToParent();
  notifyObservers();
}

inline ActorRef Actor::resolveTarget(const std::string& target) {
  if (target.empty()) return shared_from_this();
  if (target == "__parent__") return parent_.lock();
  if (auto it = children_.find(target); it != children_.end()) return it->second;
  auto sys = system_.lock();
  return sys ? sys->get(target) : nullptr;
}

inline void Actor::executeEffects(ActorScope& scope) {
  // effects may append more effects (e.g. spawn -> child start); index loop
  for (std::size_t i = 0; i < scope.effects.size(); ++i) {
    Effect effect = std::move(scope.effects[i]);
    executeEffect(effect);
  }
  scope.effects.clear();
}

inline void Actor::executeEffect(Effect& e) {
  switch (e.kind) {
    case Effect::Kind::Send: {
      ActorRef target = resolveTarget(e.target);
      if (target)
        target->send(e.event);
      else
        std::fprintf(stderr, "[xstate:%s] dropped send to unknown target '%s'\n", id_.c_str(),
                     e.target.c_str());
      break;
    }
    case Effect::Kind::StartTimer: {
      if (auto it = timers_.find(e.id); it != timers_.end())
        clock_->clearTimeout(it->second);
      std::weak_ptr<Actor> self = weak_from_this();
      const Event ev = e.event;
      const std::string target = e.target;
      const std::string sendId = e.id;
      // timer callbacks run on the loop/executor context (adapter contract),
      // so erasing the consumed timer here is race-free — and required, or a
      // later CancelTimer would "clear" an already-fired platform timer
      timers_[e.id] = clock_->setTimeout(
          [self, ev, target, sendId] {
            if (auto s = self.lock()) {
              s->timers_.erase(sendId);
              if (ActorRef t = s->resolveTarget(target)) t->send(ev);
            }
          },
          std::chrono::milliseconds(e.delayMs));
      break;
    }
    case Effect::Kind::CancelTimer: {
      if (auto it = timers_.find(e.id); it != timers_.end()) {
        clock_->clearTimeout(it->second);
        timers_.erase(it);
      }
      break;
    }
    case Effect::Kind::SpawnChild: {
      auto sys = system_.lock();
      if (!e.factory || sys == nullptr) {
        if (!e.factory)
          std::fprintf(stderr, "[xstate:%s] spawn of '%s' has no logic factory\n", id_.c_str(),
                       e.src.c_str());
        break;
      }
      ActorOptions childOpts;
      childOpts.input = e.input;
      childOpts.snapshot = e.restore;
      ActorRef child =
          sys->createActor(e.factory(e.input), e.id, weak_from_this(), std::move(childOpts));
      children_[e.id] = child;
      childMeta_[e.id] = ChildMeta{e.src, e.input};
      if (e.autoStopOnExit) autoStopChildren_.push_back(e.id);
      child->start();
      break;
    }
    case Effect::Kind::StopChild: {
      if (auto it = children_.find(e.id); it != children_.end()) {
        it->second->stop();
        children_.erase(it);
        childMeta_.erase(e.id);
      }
      break;
    }
    case Effect::Kind::Emit: {
      for (auto& [subId, entry] : emitHandlers_)
        if (entry.first == e.event.type) entry.second(e.event);
      break;
    }
    case Effect::Kind::Log: {
      std::fprintf(stderr, "[xstate:%s] %s\n", id_.c_str(), e.message.c_str());
      break;
    }
  }
}

inline void Actor::syncChildrenIntoSnapshot() {
  SnapshotPtr cur = getSnapshot();
  if (cur == nullptr) return;
  auto updated = cur->clone();
  updated->setChildren(children_);
  storeSnapshot(std::move(updated));
}

inline void Actor::notifyObservers() {
  SnapshotPtr snap = getSnapshot();
  auto observers = observers_;  // copy: callbacks may (un)subscribe
  for (auto& [subId, fn] : observers) fn(snap);
}

inline std::any Actor::getPersistedSnapshot() const {
  SnapshotPtr snap = getSnapshot();
  if (snap == nullptr) throw ConfigError("cannot persist an actor that never started");
  std::any base = logic_->getPersistedSnapshot(snap);
  if (auto* ps = std::any_cast<PersistedSnapshot>(&base)) {
    for (const auto& [cid, child] : children_) {
      PersistedSnapshot::Child pc;
      if (auto it = childMeta_.find(cid); it != childMeta_.end()) {
        pc.src = it->second.src;
        pc.input = it->second.input;
      }
      pc.snapshot = std::make_shared<PersistedSnapshot>(
          std::any_cast<PersistedSnapshot>(child->getPersistedSnapshot()));
      ps->children[cid] = std::move(pc);
    }
  }
  return base;
}

inline void Actor::reportCompletionToParent() {
  if (completionReported_) return;
  SnapshotPtr cur = getSnapshot();
  if (cur == nullptr) return;
  ActorRef p = parent_.lock();
  if (p == nullptr) return;
  if (cur->status == SnapshotStatus::Done) {
    completionReported_ = true;
    p->send(events::doneActor(id_, cur->output));
  } else if (cur->status == SnapshotStatus::Error) {
    completionReported_ = true;
    p->send(events::errorActor(id_, cur->error));
  }
}

}  // namespace xstate
