#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "actor_logic.hpp"
#include "detail/json_value.hpp"
#include "detail/json_writer.hpp"
#include "state_value.hpp"

namespace xstate {

inline std::string stateValueJson(const StateValue& v) {
  if (v.empty()) return "null";
  if (v.isLeaf()) return detail::jsonString(v.leaf());
  std::string out = "{";
  bool first = true;
  for (const auto& [key, sub] : v.branches()) {
    if (!first) out += ",";
    first = false;
    out += detail::jsonString(key) + ":" + stateValueJson(sub);
  }
  return out + "}";
}

template <typename C>
class Machine;

namespace detail {
template <typename C>
struct StateNode;
}

// Structured persisted form (v5 getPersistedSnapshot shape). Works for any
// copyable context via std::any; the JSON form additionally needs a
// ContextCodec. Children carry the logic src name so restore can respawn
// them through the machine's `actors` options.
struct PersistedSnapshot {
  SnapshotStatus status = SnapshotStatus::Active;
  std::any output;
  std::any error;
  StateValue value;
  std::any context;
  std::map<std::string, std::vector<std::string>> historyValue;  // history id -> node ids

  struct Child {
    std::string src;
    std::any input;
    std::shared_ptr<PersistedSnapshot> snapshot;
  };
  std::map<std::string, Child> children;
};

// JSON (de)serialization for a machine's context type.
template <typename C>
struct ContextCodec {
  std::function<std::string(const C&)> toJson;
  std::function<C(std::string_view)> fromJson;
};

namespace detail {

template <typename T>
void codecWriteField(std::string& out, bool& first, const char* name, const T& value) {
  if (!first) out += ",";
  first = false;
  out += jsonString(name);
  out += ":";
  if constexpr (std::is_same_v<T, std::string>) {
    out += jsonString(value);
  } else if constexpr (std::is_same_v<T, bool>) {
    out += value ? "true" : "false";
  } else if constexpr (std::is_floating_point_v<T>) {
    out += std::to_string(value);
  } else {
    static_assert(std::is_integral_v<T>, "XSTATE_CONTEXT_FIELDS supports integral, floating, bool and std::string fields");
    out += std::to_string(static_cast<long long>(value));
  }
}

template <typename T>
void codecReadField(const JsonValue& obj, const char* name, T& out) {
  const JsonValue* v = obj.find(name);
  if (v == nullptr) return;  // missing field keeps its default
  if constexpr (std::is_same_v<T, std::string>) {
    out = v->str;
  } else if constexpr (std::is_same_v<T, bool>) {
    out = v->boolean;
  } else {
    out = static_cast<T>(v->number);
  }
}

}  // namespace detail

template <typename C>
struct MachineSnapshot : Snapshot {
  StateValue value;
  C context{};
  std::vector<std::string> tags;
  std::map<std::string, ActorRef> children;

  // Internal: active nodes (document order, ancestors included), the machine
  // definition, and recorded history configurations (by history node id).
  std::vector<const detail::StateNode<C>*> configuration;
  std::shared_ptr<const Machine<C>> machine;
  std::map<std::string, std::vector<const detail::StateNode<C>*>> historyValue;

  bool matches(std::string_view dottedPath) const {
    return value.contains(StateValue::fromPath(dottedPath));
  }
  bool matches(const char* dottedPath) const {
    return matches(std::string_view{dottedPath});
  }
  bool matches(const StateValue& v) const { return value.contains(v); }

  bool hasTag(std::string_view tag) const {
    return std::find(tags.begin(), tags.end(), std::string(tag)) != tags.end();
  }

  bool can(const Event& event) const;  // defined in machine.hpp (needs Machine)

  std::shared_ptr<Snapshot> clone() const override {
    return std::make_shared<MachineSnapshot>(*this);
  }
  void setChildren(const std::map<std::string, ActorRef>& kids) override { children = kids; }
  std::string valueJson() const override { return stateValueJson(value); }
};

// Downcast helper. Returns a shared_ptr (not a raw pointer) so the result
// stays valid even when called on a temporary, e.g.
//   auto snap = machineSnapshot<Ctx>(m->getInitialSnapshot(scope, {}));
template <typename C>
std::shared_ptr<const MachineSnapshot<C>> machineSnapshot(const SnapshotPtr& p) {
  return std::dynamic_pointer_cast<const MachineSnapshot<C>>(p);
}

}  // namespace xstate

// Generates `xstate::ContextCodec<TYPE> xstateCodecFor(TYPE*)` for simple
// aggregates of integral / floating / bool / std::string fields:
//   struct Ctx { int n = 0; std::string name; };
//   XSTATE_CONTEXT_FIELDS(Ctx, n, name)
//   machine->setContextCodec(xstateCodecFor(static_cast<Ctx*>(nullptr)));
// XSTATE_CC_EXPAND forces argument re-scanning so MSVC's traditional
// preprocessor doesn't treat __VA_ARGS__ as a single token during dispatch.
#define XSTATE_CC_EXPAND(x) x
#define XSTATE_CC_FE_1(M, x) M(x)
#define XSTATE_CC_FE_2(M, x, ...) M(x) XSTATE_CC_EXPAND(XSTATE_CC_FE_1(M, __VA_ARGS__))
#define XSTATE_CC_FE_3(M, x, ...) M(x) XSTATE_CC_EXPAND(XSTATE_CC_FE_2(M, __VA_ARGS__))
#define XSTATE_CC_FE_4(M, x, ...) M(x) XSTATE_CC_EXPAND(XSTATE_CC_FE_3(M, __VA_ARGS__))
#define XSTATE_CC_FE_5(M, x, ...) M(x) XSTATE_CC_EXPAND(XSTATE_CC_FE_4(M, __VA_ARGS__))
#define XSTATE_CC_FE_6(M, x, ...) M(x) XSTATE_CC_EXPAND(XSTATE_CC_FE_5(M, __VA_ARGS__))
#define XSTATE_CC_FE_7(M, x, ...) M(x) XSTATE_CC_EXPAND(XSTATE_CC_FE_6(M, __VA_ARGS__))
#define XSTATE_CC_FE_8(M, x, ...) M(x) XSTATE_CC_EXPAND(XSTATE_CC_FE_7(M, __VA_ARGS__))
#define XSTATE_CC_GET(_1, _2, _3, _4, _5, _6, _7, _8, NAME, ...) NAME
#define XSTATE_CC_FOR_EACH(M, ...)                                                          \
  XSTATE_CC_EXPAND(XSTATE_CC_GET(__VA_ARGS__, XSTATE_CC_FE_8, XSTATE_CC_FE_7, XSTATE_CC_FE_6, \
                                 XSTATE_CC_FE_5, XSTATE_CC_FE_4, XSTATE_CC_FE_3,              \
                                 XSTATE_CC_FE_2, XSTATE_CC_FE_1)(M, __VA_ARGS__))

#define XSTATE_CC_WRITE(f) xstate::detail::codecWriteField(out__, first__, #f, v__.f);
#define XSTATE_CC_READ(f) xstate::detail::codecReadField(j__, #f, v__.f);

#define XSTATE_CONTEXT_FIELDS(TYPE, ...)                                        \
  inline xstate::ContextCodec<TYPE> xstateCodecFor(TYPE*) {                     \
    xstate::ContextCodec<TYPE> codec__;                                         \
    codec__.toJson = [](const TYPE& v__) {                                      \
      std::string out__ = "{";                                                  \
      bool first__ = true;                                                      \
      XSTATE_CC_FOR_EACH(XSTATE_CC_WRITE, __VA_ARGS__)                          \
      out__ += "}";                                                             \
      return out__;                                                             \
    };                                                                          \
    codec__.fromJson = [](std::string_view s__) {                               \
      TYPE v__{};                                                               \
      xstate::detail::JsonValue j__ = xstate::detail::parseJson(s__);           \
      XSTATE_CC_FOR_EACH(XSTATE_CC_READ, __VA_ARGS__)                           \
      return v__;                                                               \
    };                                                                          \
    return codec__;                                                             \
  }
