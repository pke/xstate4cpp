#pragma once

#include <chrono>
#include <cstdint>

#include "executor.hpp"

namespace xstate {

using TimerId = std::uint64_t;

// Timer scheduling seam (xstate's Clock). Adapters decide what "time" means:
// real OS timers (posix/win32 event loops) or virtual time (manual::TestClock).
struct Clock {
  virtual ~Clock() = default;
  virtual TimerId setTimeout(Task task, std::chrono::milliseconds delay) = 0;
  virtual void clearTimeout(TimerId id) = 0;
};

}  // namespace xstate
