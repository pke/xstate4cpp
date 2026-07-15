#pragma once

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../actor_logic.hpp"
#include "../errors.hpp"
#include "../snapshot.hpp"
#include "state_node.hpp"

namespace xstate {

template <typename C>
class Machine : public ActorLogic, public std::enable_shared_from_this<Machine<C>> {
 public:
  using Node = detail::StateNode<C>;
  using TransitionDef = typename Node::TransitionDef;
  using Snap = MachineSnapshot<C>;

  static std::shared_ptr<Machine> create(MachineConfig<C> cfg, MachineOptions<C> opts) {
    auto m = std::shared_ptr<Machine>(new Machine());
    m->config_ = std::move(cfg);
    m->options_ = std::move(opts);
    m->parse();
    return m;
  }

  const Node& root() const { return *root_; }
  const Node* getNodeById(const std::string& id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second;
  }
  const MachineOptions<C>& options() const { return options_; }
  const C& initialContext() const { return config_.context; }
  const std::string& id() const { return config_.id; }

  void setContextCodec(ContextCodec<C> codec) { codec_ = std::move(codec); }

  // ---- persistence ----

  std::any getPersistedSnapshot(SnapshotPtr snap) const override {
    auto ms = machineSnapshot<C>(snap);
    if (ms == nullptr) throw ConfigError("machine cannot persist a foreign snapshot");
    PersistedSnapshot ps;
    ps.status = ms->status;
    ps.output = ms->output;
    ps.error = ms->error;
    ps.value = ms->value;
    ps.context = ms->context;
    for (const auto& [histId, nodes] : ms->historyValue) {
      auto& ids = ps.historyValue[histId];
      for (const Node* n : nodes) ids.push_back(n->id);
    }
    // children are attached by the Actor (it owns the live refs)
    return ps;
  }

  SnapshotPtr restoreSnapshot(const std::any& persisted, ActorScope& scope) override {
    const auto* ps = std::any_cast<PersistedSnapshot>(&persisted);
    if (ps == nullptr) throw ConfigError("machine restore: expected a PersistedSnapshot");
    const C* ctx = std::any_cast<C>(&ps->context);
    if (ctx == nullptr) throw ConfigError("machine restore: context type mismatch");

    auto snap = std::make_shared<Snap>();
    snap->machine = this->shared_from_this();
    snap->status = ps->status;
    snap->output = ps->output;
    snap->error = ps->error;
    snap->context = *ctx;
    restoreConfig(root_.get(), ps->value, snap->configuration);
    std::sort(snap->configuration.begin(), snap->configuration.end(),
              [](const Node* a, const Node* b) { return a->docIndex < b->docIndex; });
    for (const auto& [histId, ids] : ps->historyValue) {
      auto& nodes = snap->historyValue[histId];
      for (const auto& nid : ids) {
        const Node* n = getNodeById(nid);
        if (n == nullptr) throw ConfigError("machine restore: unknown history node id '" + nid + "'");
        nodes.push_back(n);
      }
    }
    finalizeSnapshot(*snap);
    snap->status = ps->status;  // finalize must not clobber a persisted Done

    if (snap->status == SnapshotStatus::Active) {
      // re-arm `after` timers from the full delay
      emitDelayEffects({}, snap->configuration, snap->context, events::init(), scope);
      // respawn persisted children (restored recursively via their snapshots)
      std::set<std::string> invokeIds;
      for (const Node* n : snap->configuration)
        for (const auto& inv : n->invokes) invokeIds.insert(inv.id);
      for (const auto& [cid, child] : ps->children) {
        auto factoryIt = options_.actors.find(child.src);
        if (factoryIt == options_.actors.end())
          throw ConfigError("machine restore: unknown actor src '" + child.src + "'");
        Effect e;
        e.kind = Effect::Kind::SpawnChild;
        e.id = cid;
        e.src = child.src;
        e.input = child.input;
        e.autoStopOnExit = invokeIds.count(cid) != 0;
        if (child.snapshot) e.restore = *child.snapshot;
        e.factory = factoryIt->second;
        scope.effects.push_back(std::move(e));
      }
    }
    return snap;
  }

  std::string persistedToJson(const std::any& persisted) const override {
    const auto* ps = std::any_cast<PersistedSnapshot>(&persisted);
    if (ps == nullptr) throw ConfigError("persistedToJson: expected a PersistedSnapshot");
    if (!codec_)
      throw ConfigError(
          "JSON persistence requires a context codec (setContextCodec / XSTATE_CONTEXT_FIELDS)");
    if (!ps->children.empty())
      throw ConfigError(
          "JSON persistence with live child actors is not supported in v1; "
          "use the structured getPersistedSnapshot() form");
    const C* ctx = std::any_cast<C>(&ps->context);
    if (ctx == nullptr) throw ConfigError("persistedToJson: context type mismatch");
    std::string out = "{\"status\":\"";
    out += statusName(ps->status);
    out += "\",\"value\":" + stateValueJson(ps->value);
    out += ",\"context\":" + codec_->toJson(*ctx);
    out += ",\"historyValue\":{";
    bool firstH = true;
    for (const auto& [histId, ids] : ps->historyValue) {
      if (!firstH) out += ",";
      firstH = false;
      out += detail::jsonString(histId) + ":[";
      bool firstN = true;
      for (const auto& nid : ids) {
        if (!firstN) out += ",";
        firstN = false;
        out += detail::jsonString(nid);
      }
      out += "]";
    }
    out += "}}";
    return out;
  }

  // Parse the JSON form back into the structured form (feed the result to
  // ActorOptions::snapshot). Output/error payloads are not round-tripped.
  std::any parseSnapshotJson(std::string_view json) const {
    if (!codec_)
      throw ConfigError(
          "JSON persistence requires a context codec (setContextCodec / XSTATE_CONTEXT_FIELDS)");
    detail::JsonValue doc = detail::parseJson(json);
    if (!doc.isObject()) throw JsonError("snapshot JSON: expected an object");
    PersistedSnapshot ps;
    if (const auto* status = doc.find("status"); status && status->isString())
      ps.status = statusFromName(status->str);
    if (const auto* value = doc.find("value")) ps.value = jsonToStateValue(*value);
    if (const auto* ctx = doc.find("context"))
      ps.context = codec_->fromJson(detail::writeJson(*ctx));
    else
      ps.context = C{};
    if (const auto* hist = doc.find("historyValue"); hist && hist->isObject()) {
      for (const auto& [histId, arr] : hist->object) {
        auto& ids = ps.historyValue[histId];
        for (const auto& item : arr.array) ids.push_back(item.str);
      }
    }
    return ps;
  }

  // ---- ActorLogic ----

  SnapshotPtr getInitialSnapshot(ActorScope& scope, const std::any& /*input*/) override {
    auto snap = std::make_shared<Snap>();
    snap->machine = this->shared_from_this();
    std::vector<const Node*> cfg;
    addInitial(root_.get(), cfg);
    C draft = config_.context;
    std::deque<Event> internalQ;
    MicroCtx mc{draft, scope, &internalQ, 0};
    const Event initEvent = events::init();
    emitInvokeEffects({}, cfg, scope);  // children exist before entry sendTo effects
    for (const Node* n : cfg) executeActions(n->entry, mc, initEvent);
    emitDelayEffects({}, cfg, draft, initEvent, scope);
    snap->configuration = std::move(cfg);
    snap->context = std::move(draft);
    std::set<const Node*> active(snap->configuration.begin(), snap->configuration.end());
    processFinals(snap->configuration, active, snap->context, initEvent, internalQ, *snap);
    finalizeSnapshot(*snap);
    // settle eventless transitions / done events raised by the initial entry
    return finishIfDone(stabilize(snap, scope, internalQ), initEvent, scope);
  }

  SnapshotPtr transition(SnapshotPtr current, const Event& event, ActorScope& scope) override {
    auto cur = machineSnapshot<C>(current);
    if (cur == nullptr) throw ConfigError("machine received a foreign snapshot");
    if (cur->status != SnapshotStatus::Active) return current;
    auto selected = selectTransitions(*cur, event);
    if (selected.empty()) return current;
    std::deque<Event> internalQ;
    auto snap = microstep(selected, *cur, event, scope, internalQ);
    return finishIfDone(stabilize(snap, scope, internalQ), event, scope);
  }

  // A machine that reached its top-level final state stops: exit actions of
  // every active state run in reverse document order (root included), timers
  // are cancelled, and invoked children are stopped — matching v5.
  std::shared_ptr<const Snap> finishIfDone(std::shared_ptr<const Snap> snap, const Event& event,
                                           ActorScope& scope) {
    if (snap->status != SnapshotStatus::Done) return snap;
    auto next = std::make_shared<Snap>(*snap);
    C draft = next->context;
    std::deque<Event> discarded;  // events raised while finishing go nowhere
    MicroCtx mc{draft, scope, &discarded, 0};
    std::vector<const Node*> reverse(next->configuration.rbegin(), next->configuration.rend());
    for (const Node* n : reverse) executeActions(n->exit, mc, event);
    emitDelayEffects(reverse, {}, draft, event, scope);
    emitInvokeEffects(reverse, {}, scope);
    next->context = std::move(draft);
    return next;
  }

  // External stop (actor.stop() / parent stopping an invoked child): run the
  // exit actions of the active configuration, deepest-first.
  void onStop(SnapshotPtr current, ActorScope& scope) override {
    auto cur = machineSnapshot<C>(current);
    if (cur == nullptr || cur->status != SnapshotStatus::Active) return;
    C draft = cur->context;
    std::deque<Event> discarded;
    MicroCtx mc{draft, scope, &discarded, 0};
    const Event stopEvent{"xstate.stop"};
    for (auto it = cur->configuration.rbegin(); it != cur->configuration.rend(); ++it)
      executeActions((*it)->exit, mc, stopEvent);
  }

  bool canTransition(const Snap& snap, const Event& event) const {
    return !selectTransitions(snap, event).empty();
  }

 private:
  Machine() = default;

  std::optional<ContextCodec<C>> codec_;

  static const char* statusName(SnapshotStatus s) {
    switch (s) {
      case SnapshotStatus::Active: return "active";
      case SnapshotStatus::Done: return "done";
      case SnapshotStatus::Error: return "error";
      case SnapshotStatus::Stopped: return "stopped";
    }
    return "active";
  }

  static SnapshotStatus statusFromName(const std::string& s) {
    if (s == "done") return SnapshotStatus::Done;
    if (s == "error") return SnapshotStatus::Error;
    if (s == "stopped") return SnapshotStatus::Stopped;
    return SnapshotStatus::Active;
  }

  static StateValue jsonToStateValue(const detail::JsonValue& v) {
    if (v.isString()) return StateValue{v.str};
    if (v.isObject()) {
      std::map<std::string, StateValue> branches;
      for (const auto& [key, sub] : v.object) branches.emplace(key, jsonToStateValue(sub));
      return StateValue::branchesOf(std::move(branches));
    }
    return StateValue{};
  }

  // Rebuild the active configuration from a persisted StateValue.
  void restoreConfig(const Node* node, const StateValue& v,
                     std::vector<const Node*>& out) const {
    out.push_back(node);
    if (v.empty()) return;
    if (v.isLeaf()) {
      const Node* ch = node->childByKey(v.leaf());
      if (ch == nullptr)
        throw ConfigError("machine restore: unknown state '" + v.leaf() + "'");
      out.push_back(ch);
      return;
    }
    for (const auto& [key, sub] : v.branches()) {
      const Node* ch = node->childByKey(key);
      if (ch == nullptr) throw ConfigError("machine restore: unknown state '" + key + "'");
      restoreConfig(ch, sub, out);
    }
  }

  // Context for one microstep's action execution: the mutable draft context,
  // the effect sink, and (once macrosteps exist) the internal raised queue.
  struct MicroCtx {
    C& draft;
    ActorScope& scope;
    std::deque<Event>* internalQ;
    int sendCounter;
    bool assigned = false;  // an Assign action ran (drives eventless re-evaluation)
  };

  void executeActions(const std::vector<ActionRef<C>>& actions, MicroCtx& mc,
                      const Event& event) {
    using K = typename ActionRef<C>::Kind;
    for (const auto& a : actions) {
      switch (a.kind) {
        case K::Named:
          options_.actions.at(a.name)(mc.draft, event);
          break;
        case K::Assign:
          mc.assigned = true;
          a.fn(mc.draft, event);
          break;
        case K::Inline:
          a.fn(mc.draft, event);
          break;
        case K::Raise:
          if (a.delayMs == 0 && mc.internalQ != nullptr) {
            mc.internalQ->push_back(a.event);
          } else {
            Effect e;
            e.kind = Effect::Kind::StartTimer;
            e.id = a.sendId.empty()
                       ? "xstate.raise." + std::to_string(mc.sendCounter++)
                       : a.sendId;
            e.event = a.event;
            e.delayMs = a.delayMs;
            mc.scope.effects.push_back(std::move(e));
          }
          break;
        case K::SendTo: {
          Effect e;
          e.kind = a.delayMs == 0 ? Effect::Kind::Send : Effect::Kind::StartTimer;
          e.id = a.sendId.empty()
                     ? "xstate.send." + std::to_string(mc.sendCounter++)
                     : a.sendId;
          e.target = a.name;
          e.event = a.event;
          e.delayMs = a.delayMs;
          mc.scope.effects.push_back(std::move(e));
          break;
        }
        case K::SendParent: {
          Effect e;
          e.kind = Effect::Kind::Send;
          e.target = "__parent__";
          e.event = a.event;
          mc.scope.effects.push_back(std::move(e));
          break;
        }
        case K::Log: {
          Effect e;
          e.kind = Effect::Kind::Log;
          e.message = a.message;
          mc.scope.effects.push_back(std::move(e));
          break;
        }
        case K::Cancel: {
          Effect e;
          e.kind = Effect::Kind::CancelTimer;
          e.id = a.name;
          mc.scope.effects.push_back(std::move(e));
          break;
        }
        case K::StopChild: {
          Effect e;
          e.kind = Effect::Kind::StopChild;
          e.id = a.name;
          mc.scope.effects.push_back(std::move(e));
          break;
        }
        case K::SpawnChild: {
          Effect e;
          e.kind = Effect::Kind::SpawnChild;
          e.id = a.name.empty() ? a.src : a.name;
          e.src = a.src;
          e.input = a.input;
          e.factory = options_.actors.at(a.src);  // validated at parse
          mc.scope.effects.push_back(std::move(e));
          break;
        }
        case K::Emit: {
          Effect e;
          e.kind = Effect::Kind::Emit;
          e.event = a.event;
          mc.scope.effects.push_back(std::move(e));
          break;
        }
      }
    }
  }

  // Configuration = all active nodes (atomics + their ancestors), document order.
  void addInitial(const Node* n, std::vector<const Node*>& cfg) const {
    // history nodes never join the configuration; resolve through them
    // (no recorded value can exist on an initial path -> default target)
    if (n->type == StateType::History) {
      if (n->historyDefault != nullptr) addInitial(n->historyDefault, cfg);
      return;
    }
    cfg.push_back(n);
    if (n->type == StateType::Parallel) {
      for (const auto& ch : n->children)
        if (ch->type != StateType::History) addInitial(ch.get(), cfg);
    } else if (n->initial != nullptr) {
      addInitial(n->initial, cfg);
    }
  }

  // Value describing `node`'s active descendants ({} when node is a leaf).
  StateValue valueOf(const Node* node, const std::set<const Node*>& active) const {
    if (node->type == StateType::Parallel) {
      std::map<std::string, StateValue> branches;
      for (const auto& ch : node->children) {
        StateValue sub = valueOf(ch.get(), active);
        branches.emplace(ch->key, sub.empty() ? StateValue{} : sub);
      }
      return StateValue::branchesOf(std::move(branches));
    }
    for (const auto& ch : node->children) {
      if (active.count(ch.get()) == 0) continue;
      StateValue sub = valueOf(ch.get(), active);
      if (sub.empty()) return StateValue{ch->key};
      return StateValue::branchesOf({{ch->key, std::move(sub)}});
    }
    return StateValue{};
  }

  void finalizeSnapshot(Snap& snap) const {
    std::set<const Node*> active(snap.configuration.begin(), snap.configuration.end());
    snap.value = valueOf(root_.get(), active);
    snap.tags.clear();
    for (const Node* n : snap.configuration)
      for (const auto& t : n->tags)
        if (std::find(snap.tags.begin(), snap.tags.end(), t) == snap.tags.end())
          snap.tags.push_back(t);
  }

  // ---- transition selection ----

  bool evaluateGuard(const GuardRef<C>& g, const C& ctx, const Event& event,
                     const Snap& snap) const {
    using K = typename GuardRef<C>::Kind;
    switch (g.kind) {
      case K::Named:
        return options_.guards.at(g.name)(ctx, event);
      case K::Inline:
        return g.fn(ctx, event);
      case K::And:
        for (auto& op : g.operands)
          if (!evaluateGuard(op, ctx, event, snap)) return false;
        return true;
      case K::Or:
        for (auto& op : g.operands)
          if (evaluateGuard(op, ctx, event, snap)) return true;
        return false;
      case K::Not:
        return !evaluateGuard(g.operands.at(0), ctx, event, snap);
      case K::StateIn:
        return snap.value.contains(StateValue::fromPath(g.name));
    }
    return false;
  }

  // First enabled transition on `node` for `event`: exact event-type
  // candidates first (definition order), then wildcard "*".
  const TransitionDef* selectForNode(const Node* node, const Event& event,
                                     const Snap& snap) const {
    for (const auto& def : node->transitions) {
      if (def.eventType != event.type) continue;
      if (def.guard && !evaluateGuard(*def.guard, snap.context, event, snap)) continue;
      return &def;
    }
    if (!event.type.empty()) {  // wildcard never matches the eventless event
      for (const auto& def : node->transitions) {
        if (def.eventType != "*") continue;
        if (def.guard && !evaluateGuard(*def.guard, snap.context, event, snap)) continue;
        return &def;
      }
    }
    return nullptr;
  }

  // One transition per active atomic node (walking up through ancestors),
  // deduped and conflict-filtered (exit-set intersection; descendants win).
  std::vector<const TransitionDef*> selectTransitions(const Snap& snap,
                                                      const Event& event) const {
    std::set<const Node*> active(snap.configuration.begin(), snap.configuration.end());
    std::vector<const TransitionDef*> selected;
    for (const Node* n : snap.configuration) {
      bool isAtomic = true;  // no active child
      for (const auto& ch : n->children)
        if (active.count(ch.get()) != 0) { isAtomic = false; break; }
      if (!isAtomic) continue;
      for (const Node* s = n; s != nullptr; s = s->parent) {
        if (const TransitionDef* def = selectForNode(s, event, snap)) {
          // a forbidden entry blocks the event from bubbling further but
          // contributes no transition itself
          if (!def->forbidden &&
              std::find(selected.begin(), selected.end(), def) == selected.end())
            selected.push_back(def);
          break;
        }
      }
    }
    // Conflict resolution: drop a transition whose exit set intersects an
    // earlier-selected one's, unless its source is a descendant (then it
    // replaces the ancestor's).
    std::vector<const TransitionDef*> result;
    for (const TransitionDef* cand : selected) {
      auto candExit = computeExitSet(cand, active);
      bool dropped = false;
      for (auto it = result.begin(); it != result.end();) {
        auto keptExit = computeExitSet(*it, active);
        bool intersects = false;
        for (const Node* n : candExit)
          if (keptExit.count(n) != 0) { intersects = true; break; }
        if (!intersects) { ++it; continue; }
        if (cand->source->isDescendantOf((*it)->source) && cand->source != (*it)->source) {
          it = result.erase(it);  // descendant wins
        } else {
          dropped = true;
          break;
        }
      }
      if (!dropped) result.push_back(cand);
    }
    return result;
  }

  // ---- exit/entry set computation ----

  // Transition domain: the source itself when every target is the source or
  // one of its descendants and reenter is false (internal / default
  // self-transition: descendants re-enter, the source does not). Otherwise
  // the lowest compound PROPER ancestor of {source} U targets — targeting an
  // ancestor therefore exits and re-enters that ancestor.
  const Node* transitionDomain(const TransitionDef* def) const {
    if (def->targets.empty()) return nullptr;
    if (!def->reenter) {
      bool allInside = true;
      for (const Node* t : def->targets)
        if (!t->isDescendantOf(def->source)) { allInside = false; break; }
      if (allInside) return def->source;
    }
    for (const Node* a = def->source->parent; a != nullptr; a = a->parent) {
      bool coversAll = true;
      for (const Node* t : def->targets)
        if (t == a || !t->isDescendantOf(a)) { coversAll = false; break; }
      if (coversAll && a->type != StateType::Parallel) return a;
    }
    return root_.get();
  }

  // Active nodes strictly below the domain.
  std::set<const Node*> computeExitSet(const TransitionDef* def,
                                       const std::set<const Node*>& active) const {
    std::set<const Node*> out;
    if (def->targets.empty()) return out;
    const Node* domain = transitionDomain(def);
    for (const Node* n : active)
      if (n != domain && n->isDescendantOf(domain)) out.insert(n);
    return out;
  }

  // Push ancestors of `n` (inclusive) up to `stop` (exclusive), parents-first.
  void pushChain(const Node* stop, const Node* n, std::vector<const Node*>& entered) const {
    std::vector<const Node*> chain;
    for (const Node* a = n; a != nullptr && a != stop; a = a->parent) chain.push_back(a);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
      if (std::find(entered.begin(), entered.end(), *it) == entered.end())
        entered.push_back(*it);
  }

  using HistoryMap = std::map<std::string, std::vector<const Node*>>;

  // Chain from domain (exclusive) down to target, then initial expansion.
  // History targets resolve to their stored configuration, default target,
  // or the parent's initial state; the history node itself is never entered.
  void computeEntrySet(const Node* domain, const Node* target,
                       std::vector<const Node*>& entered, const HistoryMap& hist) const {
    if (target->type == StateType::History) {
      const Node* p = target->parent;
      pushChain(domain, p, entered);
      auto it = hist.find(target->id);
      if (it != hist.end() && !it->second.empty()) {
        for (const Node* stored : it->second) {
          pushChain(p, stored->parent, entered);
          if (std::find(entered.begin(), entered.end(), stored) == entered.end())
            addInitial(stored, entered);
        }
      } else if (target->historyDefault != nullptr) {
        computeEntrySet(domain, target->historyDefault, entered, hist);
      } else if (p->initial != nullptr) {
        computeEntrySet(domain, p->initial, entered, hist);
      }
      return;
    }
    pushChain(domain, target->parent, entered);
    if (std::find(entered.begin(), entered.end(), target) == entered.end())
      addInitial(target, entered);
  }

  // ---- delayed transitions (after) ----

  long long resolveDelay(const typename Node::DelayDef& d, const C& ctx,
                         const Event& event) const {
    if (!d.named) return d.ms;
    const auto& entry = options_.delays.at(d.key);
    if (const auto* fixed = std::get_if<long long>(&entry)) return *fixed;
    return std::get<std::function<long long(const C&, const Event&)>>(entry)(ctx, event);
  }

  void emitDelayEffects(const std::vector<const Node*>& exited,
                        const std::vector<const Node*>& entered, const C& ctx,
                        const Event& event, ActorScope& scope) const {
    for (const Node* n : exited) {
      for (const auto& d : n->delays) {
        Effect e;
        e.kind = Effect::Kind::CancelTimer;
        e.id = d.eventType;
        scope.effects.push_back(std::move(e));
      }
    }
    for (const Node* n : entered) {
      for (const auto& d : n->delays) {
        Effect e;
        e.kind = Effect::Kind::StartTimer;
        e.id = d.eventType;                 // sendId doubles as the event type
        e.event = Event{d.eventType};
        e.delayMs = resolveDelay(d, ctx, event);
        scope.effects.push_back(std::move(e));
      }
    }
  }

  // ---- invoked children ----

  void emitInvokeEffects(const std::vector<const Node*>& exited,
                         const std::vector<const Node*>& entered, ActorScope& scope) const {
    for (const Node* n : exited) {
      for (const auto& inv : n->invokes) {
        Effect e;
        e.kind = Effect::Kind::StopChild;
        e.id = inv.id;
        scope.effects.push_back(std::move(e));
      }
    }
    for (const Node* n : entered) {
      for (const auto& inv : n->invokes) {
        Effect e;
        e.kind = Effect::Kind::SpawnChild;
        e.id = inv.id;
        e.src = inv.src;
        e.input = inv.input;
        e.autoStopOnExit = true;
        e.factory = options_.actors.at(inv.src);  // validated at parse
        scope.effects.push_back(std::move(e));
      }
    }
  }

  // ---- final states / done detection ----

  static std::any resolveOutput(const std::any& raw, const C& ctx, const Event& event) {
    using OutputFn = std::function<std::any(const C&, const Event&)>;
    if (const auto* fn = std::any_cast<OutputFn>(&raw)) return (*fn)(ctx, event);
    return raw;
  }

  bool isInFinal(const Node* n, const std::set<const Node*>& active) const {
    if (n->type == StateType::Final) return active.count(n) != 0;
    if (n->type == StateType::Parallel) {
      bool anyRegion = false;
      for (const auto& ch : n->children) {
        if (ch->type == StateType::History) continue;  // ignored for done-ness
        anyRegion = true;
        if (!isInFinal(ch.get(), active)) return false;
      }
      return anyRegion;
    }
    for (const auto& ch : n->children)
      if (active.count(ch.get()) != 0) return isInFinal(ch.get(), active);
    return false;
  }

  // Raises done.state events for compound parents of newly-entered final
  // states (and parallel ancestors that thereby completed); marks the machine
  // Done when the root reaches a final state. Each done event is raised at
  // most once per microstep (several regions may finish simultaneously).
  void processFinals(const std::vector<const Node*>& entered,
                     const std::set<const Node*>& active, const C& ctx, const Event& event,
                     std::deque<Event>& internalQ, Snap& snap) const {
    std::set<std::string> raisedTypes;
    auto raiseOnce = [&](std::string type, std::any data) {
      if (raisedTypes.insert(type).second)
        internalQ.push_back(Event{std::move(type), std::move(data)});
    };
    for (const Node* f : entered) {
      if (f->type != StateType::Final) continue;
      const Node* p = f->parent;
      if (p == nullptr) continue;
      if (p == root_.get() && p->type != StateType::Parallel) {
        snap.status = SnapshotStatus::Done;
        snap.output = resolveOutput(f->output, ctx, event);
        continue;
      }
      // a final state's output is not resolved when its parent is parallel
      if (p->type != StateType::Parallel)
        raiseOnce("xstate.done.state." + p->id, resolveOutput(f->output, ctx, event));
      for (const Node* a = p; a->parent != nullptr; a = a->parent) {
        const Node* pp = a->parent;
        if (pp->type == StateType::Parallel && pp != root_.get() && isInFinal(pp, active))
          raiseOnce("xstate.done.state." + pp->id, {});
      }
    }
    // a parallel root completes when every region is in a final state
    if (snap.status == SnapshotStatus::Active && root_->type == StateType::Parallel &&
        !entered.empty() && isInFinal(root_.get(), active))
      snap.status = SnapshotStatus::Done;
  }

  // ---- macrostep loop ----

  // Run always-transitions and internally raised events until stable.
  // Eventless transitions are re-evaluated after every microstep, but a
  // targetless eventless microstep that neither changed the configuration
  // nor ran an assign stops the eventless loop (v5: run once per event, not
  // forever) until the next queued event is processed.
  std::shared_ptr<const Snap> stabilize(std::shared_ptr<const Snap> snap, ActorScope& scope,
                                        std::deque<Event>& internalQ) {
    const Event eventless{""};
    std::size_t steps = 0;
    bool alwaysEnabled = true;
    while (true) {
      if (snap->status != SnapshotStatus::Active) return snap;
      if (++steps > 1000000)
        throw ConfigError("infinite loop detected in always transitions");
      if (alwaysEnabled) {
        auto alwaysSel = selectTransitions(*snap, eventless);
        if (!alwaysSel.empty()) {
          bool progress = false;
          snap = microstep(alwaysSel, *snap, eventless, scope, internalQ, &progress);
          if (!progress) alwaysEnabled = false;
          continue;
        }
      }
      if (!internalQ.empty()) {
        Event e = std::move(internalQ.front());
        internalQ.pop_front();
        auto sel = selectTransitions(*snap, e);
        if (!sel.empty()) snap = microstep(sel, *snap, e, scope, internalQ);
        alwaysEnabled = true;  // a processed event re-arms eventless evaluation
        continue;
      }
      return snap;
    }
  }

  // ---- microstep execution ----

  std::shared_ptr<const Snap> microstep(const std::vector<const TransitionDef*>& selected,
                                        const Snap& cur, const Event& event, ActorScope& scope,
                                        std::deque<Event>& internalQ,
                                        bool* madeProgress = nullptr) {
    std::set<const Node*> active(cur.configuration.begin(), cur.configuration.end());
    C draft = cur.context;
    MicroCtx mc{draft, scope, &internalQ, 0};

    // exit set: union over selected transitions, deepest-first
    std::set<const Node*> exitSet;
    for (const TransitionDef* def : selected)
      for (const Node* n : computeExitSet(def, active)) exitSet.insert(n);
    std::vector<const Node*> exitOrdered(exitSet.begin(), exitSet.end());
    std::sort(exitOrdered.begin(), exitOrdered.end(),
              [](const Node* a, const Node* b) { return a->docIndex > b->docIndex; });

    // record history for exited nodes that have history children (captured
    // against the pre-exit configuration)
    HistoryMap newHistory = cur.historyValue;
    for (const Node* n : exitOrdered) {
      for (const auto& hc : n->children) {
        if (hc->type != StateType::History) continue;
        std::vector<const Node*> rec;
        if (hc->history == HistoryType::Shallow) {
          for (const auto& ch : n->children)
            if (active.count(ch.get()) != 0) rec.push_back(ch.get());
        } else {
          for (const Node* a : active) {
            if (a == n || !a->isDescendantOf(n)) continue;
            bool atomic = true;
            for (const auto& ch : a->children)
              if (active.count(ch.get()) != 0) { atomic = false; break; }
            if (atomic) rec.push_back(a);
          }
        }
        newHistory[hc->id] = std::move(rec);
      }
    }

    for (const Node* n : exitOrdered) executeActions(n->exit, mc, event);
    // cancel timers and stop invoked children of exited states (after their
    // exit actions so those actions can still talk to the children)
    emitDelayEffects(exitOrdered, {}, draft, event, scope);
    emitInvokeEffects(exitOrdered, {}, scope);

    // transition actions, selection order
    for (const TransitionDef* def : selected) executeActions(def->actions, mc, event);

    // entry set
    std::vector<const Node*> entered;
    for (const TransitionDef* def : selected) {
      if (def->targets.empty()) continue;
      const Node* domain = transitionDomain(def);
      for (const Node* t : def->targets) computeEntrySet(domain, t, entered, newHistory);
    }

    // new configuration = (active - exited) + entered (deduped), doc order
    std::set<const Node*> nextActive;
    for (const Node* n : active)
      if (exitSet.count(n) == 0) nextActive.insert(n);
    std::vector<const Node*> toEnter;
    for (const Node* n : entered)
      if (nextActive.insert(n).second) toEnter.push_back(n);

    // configuration completion: an active compound needs an active child, an
    // active parallel needs ALL regions active (e.g. a cross-region target
    // exited the source region — it re-enters via its initial states)
    completeConfiguration(nextActive, toEnter);
    std::sort(toEnter.begin(), toEnter.end(),
              [](const Node* a, const Node* b) { return a->docIndex < b->docIndex; });

    // invoked children spawn before entry actions execute so that entry
    // sendTo(childId, ...) effects find their target
    emitInvokeEffects({}, toEnter, scope);
    for (const Node* n : toEnter) executeActions(n->entry, mc, event);
    emitDelayEffects({}, toEnter, draft, event, scope);

    auto next = std::make_shared<Snap>();
    next->machine = this->shared_from_this();
    next->configuration.assign(nextActive.begin(), nextActive.end());
    std::sort(next->configuration.begin(), next->configuration.end(),
              [](const Node* a, const Node* b) { return a->docIndex < b->docIndex; });
    next->context = std::move(draft);
    next->children = cur.children;
    next->historyValue = std::move(newHistory);
    processFinals(toEnter, nextActive, next->context, event, internalQ, *next);
    finalizeSnapshot(*next);
    if (madeProgress != nullptr)
      *madeProgress = !exitSet.empty() || !toEnter.empty() || mc.assigned;
    return next;
  }

  // Expand incomplete compound/parallel nodes down to their initial states,
  // treating everything added as freshly entered.
  void completeConfiguration(std::set<const Node*>& nextActive,
                             std::vector<const Node*>& toEnter) const {
    bool changed = true;
    while (changed) {
      changed = false;
      std::vector<const Node*> snapshot(nextActive.begin(), nextActive.end());
      for (const Node* n : snapshot) {
        std::vector<const Node*> add;
        if (n->type == StateType::Parallel) {
          for (const auto& region : n->children)
            if (nextActive.count(region.get()) == 0) addInitial(region.get(), add);
        } else if (n->type == StateType::Compound && n->initial != nullptr) {
          bool hasActiveChild = false;
          for (const auto& ch : n->children)
            if (nextActive.count(ch.get()) != 0) { hasActiveChild = true; break; }
          if (!hasActiveChild) addInitial(n->initial, add);
        }
        for (const Node* a : add)
          if (nextActive.insert(a).second) {
            toEnter.push_back(a);
            changed = true;
          }
      }
    }
  }

  MachineConfig<C> config_;
  MachineOptions<C> options_;
  std::unique_ptr<Node> root_;
  std::map<std::string, Node*> byId_;

  static bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char ch) {
      return std::isdigit(ch) != 0;
    });
  }

  [[noreturn]] static void fail(const std::string& cfgPath, const std::string& what) {
    throw ConfigError(cfgPath + ": " + what);
  }

  void registerId(Node* node, const std::string& cfgPath) {
    if (!byId_.emplace(node->id, node).second)
      fail(cfgPath, "duplicate state id '" + node->id + "'");
  }

  // ---- pass 1: build the node tree (no transition resolution yet) ----

  int docCounter_ = 0;

  void parse() {
    root_ = std::make_unique<Node>();
    root_->key = "";
    root_->path = "";
    root_->docIndex = docCounter_++;
    root_->id = config_.id;
    if (config_.type == StateType::Parallel) {
      if (config_.states.empty()) fail("type", "parallel machine needs states");
      root_->type = StateType::Parallel;
    } else {
      root_->type = config_.states.empty() ? StateType::Atomic : StateType::Compound;
    }
    root_->entry = config_.entry;
    root_->exit = config_.exit;
    root_->output = config_.output;
    registerId(root_.get(), "id");
    for (auto& [key, sc] : config_.states)
      root_->children.push_back(buildNode(key, sc, root_.get(), "states." + key));

    // pass 2: resolve + validate against the complete tree
    wireRoot();
  }

  std::unique_ptr<Node> buildNode(const std::string& key, StateConfig<C>& sc,
                                  Node* parent, const std::string& cfgPath) {
    auto node = std::make_unique<Node>();
    node->key = key;
    node->parent = parent;
    node->docIndex = docCounter_++;
    node->path = parent->path.empty() ? key : parent->path + "." + key;
    node->type = sc.type;
    if (node->type == StateType::Atomic && !sc.states.empty())
      node->type = StateType::Compound;
    if (node->type == StateType::Final && !sc.states.empty())
      fail(cfgPath, "final state cannot have child states");
    node->history = sc.history;
    node->entry = sc.entry;
    node->exit = sc.exit;
    node->tags = sc.tags;
    node->output = sc.output;
    node->id = sc.id.empty() ? config_.id + "." + node->path : sc.id;
    registerId(node.get(), cfgPath);
    for (auto& [ck, csc] : sc.states)
      node->children.push_back(
          buildNode(ck, csc, node.get(), cfgPath + ".states." + ck));
    return node;
  }

  // ---- pass 2: transitions, initial states, validation ----

  void wireRoot() {
    std::size_t i = 0;
    wireCommon(root_.get(), config_.initial, config_.on, config_.always, "");
    for (auto& [key, sc] : config_.states)
      wireNode(root_->children[i++].get(), sc, "states." + key);
  }

  void wireNode(Node* node, StateConfig<C>& sc, const std::string& cfgPath) {
    wireCommon(node, sc.initial, sc.on, sc.always, cfgPath);

    // history default target
    if (node->type == StateType::History && !sc.target.empty())
      node->historyDefault = resolveTarget(node, sc.target, cfgPath + ".target");

    for (auto& tc : sc.onDone)
      addTransition(node, "xstate.done.state." + node->id, tc, cfgPath + ".onDone");

    validateActions(node->entry, cfgPath + ".entry");
    validateActions(node->exit, cfgPath + ".exit");

    // after -> delayed transitions
    for (auto& [key, list] : sc.after.entries) {
      typename Node::DelayDef d;
      d.key = key;
      d.named = !allDigits(key);
      if (d.named) {
        if (options_.delays.find(key) == options_.delays.end())
          fail(cfgPath + ".after." + key, "unknown delay '" + key + "'");
      } else {
        d.ms = std::stoll(key);
      }
      d.eventType = "xstate.after(" + key + ")#" + node->id;
      node->delays.push_back(d);
      for (auto& tc : list.list)
        addTransition(node, d.eventType, tc, cfgPath + ".after." + key);
    }

    // invoke
    for (auto& inv : sc.invoke) {
      InvokeConfig<C> resolved = inv;
      if (resolved.id.empty()) resolved.id = resolved.src;
      if (options_.actors.find(resolved.src) == options_.actors.end())
        fail(cfgPath + ".invoke", "unknown actor '" + resolved.src + "'");
      for (auto& tc : resolved.onDone)
        addTransition(node, "xstate.done.actor." + resolved.id, tc, cfgPath + ".invoke.onDone");
      for (auto& tc : resolved.onError)
        addTransition(node, "xstate.error.actor." + resolved.id, tc, cfgPath + ".invoke.onError");
      node->invokes.push_back(std::move(resolved));
    }

    std::size_t i = 0;
    for (auto& [ck, csc] : sc.states)
      wireNode(node->children[i++].get(), csc, cfgPath + ".states." + ck);
  }

  void wireCommon(Node* node, const std::string& initialName,
                  OrderedMap<TransitionList<C>>& on,
                  std::vector<TransitionConfig<C>>& always,
                  const std::string& cfgPath) {
    const std::string prefix = cfgPath.empty() ? "" : cfgPath + ".";
    if (node->type == StateType::Compound && !node->children.empty()) {
      if (initialName.empty())
        fail(cfgPath.empty() ? "initial" : cfgPath, "missing initial state");
      node->initial = node->childByKey(initialName);
      if (node->initial == nullptr)
        fail(prefix + "initial", "unknown initial state '" + initialName + "'");
    }
    for (auto& [evt, list] : on) {
      if (list.list.empty()) {
        // forbidden event (v5 `EVENT: undefined`): selected but does nothing,
        // which blocks the event from bubbling to ancestor handlers
        TransitionDef def;
        def.eventType = evt;
        def.forbidden = true;
        def.source = node;
        node->transitions.push_back(std::move(def));
        continue;
      }
      for (auto& tc : list.list)
        addTransition(node, evt, tc, prefix + "on." + evt);
    }
    for (auto& tc : always)
      addTransition(node, "", tc, prefix + "always");
  }

  void addTransition(Node* node, const std::string& eventType,
                     TransitionConfig<C>& tc, const std::string& cfgPath) {
    TransitionDef def;
    def.eventType = eventType;
    def.source = node;
    def.reenter = tc.reenter;
    def.actions = tc.actions;
    def.guard = tc.guard;
    if (def.guard) validateGuard(*def.guard, cfgPath);
    validateActions(def.actions, cfgPath);
    for (auto& t : tc.targets)
      def.targets.push_back(resolveTarget(node, t, cfgPath));
    node->transitions.push_back(std::move(def));
  }

  void validateGuard(const GuardRef<C>& g, const std::string& cfgPath) {
    using K = typename GuardRef<C>::Kind;
    if (g.kind == K::Named && options_.guards.find(g.name) == options_.guards.end())
      fail(cfgPath, "unknown guard '" + g.name + "'");
    for (auto& op : g.operands) validateGuard(op, cfgPath);
  }

  void validateActions(const std::vector<ActionRef<C>>& actions,
                       const std::string& cfgPath) {
    using K = typename ActionRef<C>::Kind;
    for (auto& a : actions) {
      if (a.kind == K::Named && options_.actions.find(a.name) == options_.actions.end())
        fail(cfgPath, "unknown action '" + a.name + "'");
      if (a.kind == K::SpawnChild && options_.actors.find(a.src) == options_.actors.end())
        fail(cfgPath, "unknown actor '" + a.src + "'");
    }
  }

  static std::vector<std::string> splitDots(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
      auto dot = s.find('.', start);
      if (dot == std::string::npos) {
        out.push_back(s.substr(start));
        return out;
      }
      out.push_back(s.substr(start, dot - start));
      start = dot + 1;
    }
  }

  Node* walkDown(Node* from, const std::vector<std::string>& segs, std::size_t first,
                 const std::string& raw, const std::string& cfgPath) {
    Node* n = from;
    for (std::size_t i = first; i < segs.size(); ++i) {
      n = n->childByKey(segs[i]);
      if (n == nullptr) fail(cfgPath, "unknown target '" + raw + "'");
    }
    return n;
  }

  Node* resolveTarget(Node* source, const std::string& raw, const std::string& cfgPath) {
    if (raw.empty()) fail(cfgPath, "empty transition target");
    if (raw[0] == '#') {
      const std::string rest = raw.substr(1);
      // exact id match first (ids may themselves contain dots)
      if (auto it = byId_.find(rest); it != byId_.end()) return it->second;
      // longest id prefix + child-key walk
      auto segs = splitDots(rest);
      for (std::size_t cut = segs.size(); cut > 0; --cut) {
        std::string prefix = segs[0];
        for (std::size_t i = 1; i < cut; ++i) prefix += "." + segs[i];
        if (auto it = byId_.find(prefix); it != byId_.end())
          return walkDown(it->second, segs, cut, raw, cfgPath);
      }
      fail(cfgPath, "unknown target '" + raw + "'");
    }
    if (raw[0] == '.') {  // child of source
      auto segs = splitDots(raw.substr(1));
      return walkDown(source, segs, 0, raw, cfgPath);
    }
    // sibling scope: parent's children (root's own children for root)
    Node* scope = source->parent != nullptr ? source->parent : source;
    auto segs = splitDots(raw);
    Node* first = scope->childByKey(segs[0]);
    if (first == nullptr) fail(cfgPath, "unknown target '" + raw + "'");
    return walkDown(first, segs, 1, raw, cfgPath);
  }
};

template <typename C>
std::shared_ptr<Machine<C>> createMachine(MachineConfig<C> cfg, MachineOptions<C> opts = {}) {
  return Machine<C>::create(std::move(cfg), std::move(opts));
}

template <typename C>
bool MachineSnapshot<C>::can(const Event& event) const {
  return machine != nullptr && machine->canTransition(*this, event);
}

}  // namespace xstate
