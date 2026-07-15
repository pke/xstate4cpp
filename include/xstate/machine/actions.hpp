#pragma once

#include <any>
#include <functional>
#include <string>
#include <utility>

#include "../event.hpp"

namespace xstate {

// Action description (data, not execution). MachineLogic collects these
// during a transition; the Actor executes the resulting effects. This is
// the v5 built-in action set.
template <typename C>
struct ActionRef {
  enum class Kind {
    Named,       // options lookup
    Inline,      // fn on draft context
    Assign,      // fn on draft context (sugar kept distinct for JSON parity)
    Raise,       // internal event (delayMs == 0) or delayed self-send
    SendTo,      // name = target (child id / systemId)
    SendParent,
    Log,
    Cancel,      // name = sendId to cancel
    StopChild,   // name = child id
    SpawnChild,  // src + input, name = child id
    Emit,
  };

  Kind kind = Kind::Named;
  std::string name;
  std::function<void(C&, const Event&)> fn;  // Inline & Assign (mutable draft)
  Event event;                               // Raise/SendTo/SendParent/Emit
  long long delayMs = 0;                     // Raise/SendTo optional delay
  std::string sendId;                        // Raise/SendTo id (for cancel)
  std::string message;                       // Log
  std::string src;                           // SpawnChild logic name
  std::any input;                            // SpawnChild input
};

template <typename C>
ActionRef<C> action(std::string named) {
  ActionRef<C> a;
  a.kind = ActionRef<C>::Kind::Named;
  a.name = std::move(named);
  return a;
}

template <typename C>
ActionRef<C> assign(std::function<void(C&, const Event&)> fn) {
  ActionRef<C> a;
  a.kind = ActionRef<C>::Kind::Assign;
  a.fn = std::move(fn);
  return a;
}

template <typename C>
ActionRef<C> raise(Event e, long long delayMs = 0, std::string id = "") {
  ActionRef<C> a;
  a.kind = ActionRef<C>::Kind::Raise;
  a.event = std::move(e);
  a.delayMs = delayMs;
  a.sendId = std::move(id);
  return a;
}

template <typename C>
ActionRef<C> sendTo(std::string target, Event e, long long delayMs = 0, std::string id = "") {
  ActionRef<C> a;
  a.kind = ActionRef<C>::Kind::SendTo;
  a.name = std::move(target);
  a.event = std::move(e);
  a.delayMs = delayMs;
  a.sendId = std::move(id);
  return a;
}

template <typename C>
ActionRef<C> sendParent(Event e) {
  ActionRef<C> a;
  a.kind = ActionRef<C>::Kind::SendParent;
  a.event = std::move(e);
  return a;
}

template <typename C>
ActionRef<C> log(std::string message) {
  ActionRef<C> a;
  a.kind = ActionRef<C>::Kind::Log;
  a.message = std::move(message);
  return a;
}

template <typename C>
ActionRef<C> cancel(std::string sendId) {
  ActionRef<C> a;
  a.kind = ActionRef<C>::Kind::Cancel;
  a.name = std::move(sendId);
  return a;
}

template <typename C>
ActionRef<C> stopChild(std::string childId) {
  ActionRef<C> a;
  a.kind = ActionRef<C>::Kind::StopChild;
  a.name = std::move(childId);
  return a;
}

template <typename C>
ActionRef<C> spawnChild(std::string src, std::string id = "", std::any input = {}) {
  ActionRef<C> a;
  a.kind = ActionRef<C>::Kind::SpawnChild;
  a.src = std::move(src);
  a.name = std::move(id);
  a.input = std::move(input);
  return a;
}

template <typename C>
ActionRef<C> emit(Event e) {
  ActionRef<C> a;
  a.kind = ActionRef<C>::Kind::Emit;
  a.event = std::move(e);
  return a;
}

}  // namespace xstate
