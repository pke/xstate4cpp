#pragma once

#include <functional>

namespace xstate {

using Task = std::function<void()>;

// Serialized task queue. One Executor per ActorSystem: every actor in the
// system processes events on it, which reproduces JS event-loop guarantees
// (run-to-completion, no data races on context). post() must be safe to call
// from any thread the adapter supports.
struct Executor {
  virtual ~Executor() = default;
  virtual void post(Task task) = 0;
};

}  // namespace xstate
