#pragma once

#include <cstddef>
#include <map>
#include <utility>
#include <vector>

#include "../../interfaces/clock.hpp"

namespace xstate {
namespace manual {

// Virtual-time clock: timers fire deterministically inside advance(), in
// deadline order (ties broken by creation order). Timers created while
// advancing are relative to the current virtual time. NOT thread-safe.
class TestClock : public Clock {
 public:
  TimerId setTimeout(Task task, std::chrono::milliseconds delay) override {
    TimerId id = nextId_++;
    timers_.emplace(now_ + delay, Entry{id, std::move(task)});
    return id;
  }

  void clearTimeout(TimerId id) override {
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
      if (it->second.id == id) {
        timers_.erase(it);
        return;
      }
    }
  }

  void advance(std::chrono::milliseconds delta) {
    const auto end = now_ + delta;
    while (true) {
      auto it = timers_.begin();
      if (it == timers_.end() || it->first > end) break;
      now_ = it->first;  // timers created by this task are relative to its deadline
      Task task = std::move(it->second.task);
      timers_.erase(it);
      task();
    }
    now_ = end;
  }

  std::chrono::milliseconds now() const { return now_; }
  std::size_t pendingTimers() const { return timers_.size(); }

 private:
  struct Entry {
    TimerId id;
    Task task;
  };
  std::chrono::milliseconds now_{0};
  std::multimap<std::chrono::milliseconds, Entry> timers_;  // deadline -> entry
  TimerId nextId_ = 1;
};

}  // namespace manual
}  // namespace xstate
