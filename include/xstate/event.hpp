#pragma once

#include <any>
#include <string>
#include <utility>

namespace xstate {

struct Event {
  std::string type;
  std::any data;

  Event() = default;
  Event(std::string t) : type(std::move(t)) {}
  Event(std::string t, std::any d) : type(std::move(t)), data(std::move(d)) {}

  template <typename T>
  const T* dataAs() const {
    return std::any_cast<T>(&data);
  }
};

namespace events {

inline Event init() { return Event{"xstate.init"}; }

inline Event doneState(const std::string& stateId) {
  return Event{"xstate.done.state." + stateId};
}

inline Event doneActor(const std::string& actorId, std::any output) {
  return Event{"xstate.done.actor." + actorId, std::move(output)};
}

inline Event errorActor(const std::string& actorId, std::any error) {
  return Event{"xstate.error.actor." + actorId, std::move(error)};
}

inline std::string afterEventType(long long ms, const std::string& stateId) {
  return "xstate.after(" + std::to_string(ms) + ")#" + stateId;
}

}  // namespace events
}  // namespace xstate
