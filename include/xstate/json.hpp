#pragma once

// OPTIONAL machine-config JSON import — include only if you use it.
// Accepts the same JSON shape as xstate's createMachine() / Stately Studio
// exports. JSON can only carry *named references* (guards, actions, actors,
// delays); bind implementations via MachineOptions at createMachine time —
// unbound names throw ConfigError there, unknown JSON constructs throw
// JsonError here.

#include <string_view>

#include "detail/json_value.hpp"
#include "machine/config.hpp"
#include "snapshot.hpp"  // ContextCodec

namespace xstate {
namespace detail {

template <typename C>
GuardRef<C> jsonToGuard(const JsonValue& v) {
  if (v.isString()) return guardNamed<C>(v.str);
  if (!v.isObject()) throw JsonError("guard: expected string or object");
  const JsonValue* type = v.find("type");
  if (type == nullptr || !type->isString()) throw JsonError("guard object needs a string 'type'");
  const std::string& t = type->str;
  auto operandsOf = [&](const char* key) {
    std::vector<GuardRef<C>> ops;
    if (const JsonValue* arr = v.find(key); arr && arr->isArray())
      for (const auto& item : arr->array) ops.push_back(jsonToGuard<C>(item));
    return ops;
  };
  if (t == "xstate.guard.and") return and_<C>(operandsOf("guards"));
  if (t == "xstate.guard.or") return or_<C>(operandsOf("guards"));
  if (t == "xstate.guard.not") {
    auto ops = operandsOf("guards");
    if (ops.size() != 1) throw JsonError("xstate.guard.not needs exactly one operand");
    return not_<C>(std::move(ops[0]));
  }
  if (t == "xstate.guard.stateIn") {
    const JsonValue* path = v.find("stateIn");
    if (path == nullptr || !path->isString())
      throw JsonError("xstate.guard.stateIn needs a string 'stateIn'");
    return stateIn<C>(path->str);
  }
  if (t.rfind("xstate.", 0) == 0) throw JsonError("unknown guard type '" + t + "'");
  return guardNamed<C>(t);
}

inline Event jsonToEvent(const JsonValue& v) {
  if (!v.isObject()) throw JsonError("event: expected an object with 'type'");
  const JsonValue* type = v.find("type");
  if (type == nullptr || !type->isString()) throw JsonError("event needs a string 'type'");
  Event e{type->str};
  if (v.object.size() > 1) e.data = writeJson(v);  // raw JSON for non-trivial payloads
  return e;
}

template <typename C>
ActionRef<C> jsonToAction(const JsonValue& v) {
  if (v.isString()) return action<C>(v.str);
  if (!v.isObject()) throw JsonError("action: expected string or object");
  const JsonValue* type = v.find("type");
  if (type == nullptr || !type->isString()) throw JsonError("action object needs a string 'type'");
  const std::string& t = type->str;
  auto stringField = [&](const char* key) -> std::string {
    const JsonValue* f = v.find(key);
    return f != nullptr && f->isString() ? f->str : std::string{};
  };
  auto delayField = [&]() -> long long {
    const JsonValue* f = v.find("delay");
    return f != nullptr && f->isNumber() ? static_cast<long long>(f->number) : 0;
  };
  auto eventField = [&]() -> Event {
    const JsonValue* f = v.find("event");
    if (f == nullptr) throw JsonError("action '" + t + "' needs an 'event'");
    return jsonToEvent(*f);
  };
  if (t == "xstate.raise") return raise<C>(eventField(), delayField(), stringField("id"));
  if (t == "xstate.sendTo")
    return sendTo<C>(stringField("to"), eventField(), delayField(), stringField("id"));
  if (t == "xstate.sendParent") return sendParent<C>(eventField());
  if (t == "xstate.log") return log<C>(stringField("message"));
  if (t == "xstate.cancel") return cancel<C>(stringField("sendId"));
  if (t == "xstate.stopChild") return stopChild<C>(stringField("id"));
  if (t == "xstate.emit") return emit<C>(eventField());
  if (t == "xstate.spawnChild")
    return spawnChild<C>(stringField("src"), stringField("id"));
  if (t.rfind("xstate.", 0) == 0) throw JsonError("unknown action type '" + t + "'");
  return action<C>(t);
}

template <typename C>
std::vector<ActionRef<C>> jsonToActions(const JsonValue& v) {
  std::vector<ActionRef<C>> out;
  if (v.isArray())
    for (const auto& item : v.array) out.push_back(jsonToAction<C>(item));
  else
    out.push_back(jsonToAction<C>(v));
  return out;
}

template <typename C>
TransitionConfig<C> jsonToTransition(const JsonValue& v) {
  TransitionConfig<C> t;
  if (v.isString()) {
    t.targets.push_back(v.str);
    return t;
  }
  if (!v.isObject()) throw JsonError("transition: expected string or object");
  if (const JsonValue* target = v.find("target")) {
    if (target->isString()) {
      t.targets.push_back(target->str);
    } else if (target->isArray()) {
      for (const auto& item : target->array) t.targets.push_back(item.str);
    } else if (!target->isNull()) {
      throw JsonError("transition target: expected string, array or null");
    }
  }
  if (const JsonValue* guard = v.find("guard")) t.guard = jsonToGuard<C>(*guard);
  if (const JsonValue* actions = v.find("actions")) t.actions = jsonToActions<C>(*actions);
  if (const JsonValue* reenter = v.find("reenter")) t.reenter = reenter->boolean;
  if (const JsonValue* desc = v.find("description"); desc && desc->isString())
    t.description = desc->str;
  return t;
}

template <typename C>
void jsonToTransitionList(const JsonValue& v, TransitionList<C>& out) {
  if (v.isArray())
    for (const auto& item : v.array) out.add(jsonToTransition<C>(item));
  else
    out.add(jsonToTransition<C>(v));
}

template <typename C>
std::vector<TransitionConfig<C>> jsonToTransitionVec(const JsonValue& v) {
  std::vector<TransitionConfig<C>> out;
  if (v.isArray())
    for (const auto& item : v.array) out.push_back(jsonToTransition<C>(item));
  else
    out.push_back(jsonToTransition<C>(v));
  return out;
}

template <typename C>
InvokeConfig<C> jsonToInvoke(const JsonValue& v) {
  if (!v.isObject()) throw JsonError("invoke: expected an object");
  InvokeConfig<C> inv;
  if (const JsonValue* src = v.find("src"); src && src->isString()) inv.src = src->str;
  if (inv.src.empty()) throw JsonError("invoke needs a string 'src'");
  if (const JsonValue* id = v.find("id"); id && id->isString()) inv.id = id->str;
  if (const JsonValue* input = v.find("input")) inv.input = writeJson(*input);
  if (const JsonValue* onDone = v.find("onDone")) inv.onDone = jsonToTransitionVec<C>(*onDone);
  if (const JsonValue* onError = v.find("onError")) inv.onError = jsonToTransitionVec<C>(*onError);
  return inv;
}

template <typename C>
void jsonToState(const JsonValue& v, StateConfig<C>& out) {
  if (!v.isObject()) throw JsonError("state: expected an object");
  for (const auto& [key, val] : v.object) {
    if (key == "type") {
      if (val.str == "atomic") out.type = StateType::Atomic;
      else if (val.str == "compound") out.type = StateType::Compound;
      else if (val.str == "parallel") out.type = StateType::Parallel;
      else if (val.str == "final") out.type = StateType::Final;
      else if (val.str == "history") out.type = StateType::History;
      else throw JsonError("unknown state type '" + val.str + "'");
    } else if (key == "history") {
      if (val.str == "shallow") out.history = HistoryType::Shallow;
      else if (val.str == "deep") out.history = HistoryType::Deep;
      else throw JsonError("unknown history kind '" + val.str + "'");
    } else if (key == "id") {
      out.id = val.str;
    } else if (key == "initial") {
      out.initial = val.str;
    } else if (key == "target") {
      out.target = val.str;
    } else if (key == "entry") {
      out.entry = jsonToActions<C>(val);
    } else if (key == "exit") {
      out.exit = jsonToActions<C>(val);
    } else if (key == "on") {
      for (const auto& [evt, tv] : val.object) jsonToTransitionList<C>(tv, out.on[evt]);
    } else if (key == "after") {
      for (const auto& [delay, tv] : val.object)
        jsonToTransitionList<C>(tv, out.after[delay]);
    } else if (key == "always") {
      out.always = jsonToTransitionVec<C>(val);
    } else if (key == "onDone") {
      out.onDone = jsonToTransitionVec<C>(val);
    } else if (key == "invoke") {
      if (val.isArray())
        for (const auto& item : val.array) out.invoke.push_back(jsonToInvoke<C>(item));
      else
        out.invoke.push_back(jsonToInvoke<C>(val));
    } else if (key == "states") {
      for (const auto& [childKey, childVal] : val.object)
        jsonToState<C>(childVal, out.states[childKey]);
    } else if (key == "tags") {
      if (val.isArray())
        for (const auto& item : val.array) out.tags.push_back(item.str);
      else
        out.tags.push_back(val.str);
    } else if (key == "output") {
      out.output = writeJson(val);
    } else if (key == "meta" || key == "description") {
      // ignored in v1
    } else {
      throw JsonError("unknown state key '" + key + "'");
    }
  }
}

}  // namespace detail

// Parse an xstate/Stately machine-config JSON document. If the document has
// a "context" object, pass a ContextCodec to decode it — otherwise it throws.
template <typename C>
MachineConfig<C> parseMachineJson(std::string_view json,
                                  const ContextCodec<C>* codec = nullptr) {
  detail::JsonValue doc = detail::parseJson(json);
  if (!doc.isObject()) throw JsonError("machine config: expected a JSON object");
  MachineConfig<C> cfg;
  for (const auto& [key, val] : doc.object) {
    if (key == "id") {
      cfg.id = val.str;
    } else if (key == "initial") {
      cfg.initial = val.str;
    } else if (key == "context") {
      if (codec == nullptr)
        throw JsonError("machine config has 'context' but no ContextCodec was provided");
      cfg.context = codec->fromJson(detail::writeJson(val));
    } else if (key == "states") {
      for (const auto& [childKey, childVal] : val.object)
        detail::jsonToState<C>(childVal, cfg.states[childKey]);
    } else if (key == "on") {
      for (const auto& [evt, tv] : val.object) detail::jsonToTransitionList<C>(tv, cfg.on[evt]);
    } else if (key == "always") {
      cfg.always = detail::jsonToTransitionVec<C>(val);
    } else if (key == "entry") {
      cfg.entry = detail::jsonToActions<C>(val);
    } else if (key == "exit") {
      cfg.exit = detail::jsonToActions<C>(val);
    } else if (key == "output") {
      cfg.output = detail::writeJson(val);
    } else if (key == "meta" || key == "description" || key == "version" || key == "types") {
      // ignored in v1 (Stately exports carry these)
    } else {
      throw JsonError("unknown machine config key '" + key + "'");
    }
  }
  return cfg;
}

}  // namespace xstate
