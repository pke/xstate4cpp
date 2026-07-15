#pragma once

#ifndef _WIN32
#error "xstate/adapters/win32/event_loop.hpp requires Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#define XSTATE_DEFINED_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#define XSTATE_DEFINED_NOMINMAX
#endif
#include <windows.h>
#ifdef XSTATE_DEFINED_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#undef XSTATE_DEFINED_LEAN_AND_MEAN
#endif
#ifdef XSTATE_DEFINED_NOMINMAX
#undef NOMINMAX
#undef XSTATE_DEFINED_NOMINMAX
#endif

#include <deque>
#include <map>
#include <utility>

#include "../../interfaces/clock.hpp"
#include "../../interfaces/executor.hpp"

namespace xstate {
namespace win32 {

// One Win32 thread running a combined task queue + timer wheel. Implements
// both Executor and Clock so a whole ActorSystem runs on this single thread.
// post/setTimeout/clearTimeout are thread-safe (SRWLOCK + CONDITION_VARIABLE).
// The destructor stops the loop, drains already-queued tasks, discards
// pending timers, and joins.
class EventLoop : public Executor, public Clock {
 public:
  EventLoop() {
    InitializeSRWLock(&lock_);
    InitializeConditionVariable(&cond_);
    thread_ = ::CreateThread(nullptr, 0, &EventLoop::trampoline, this, 0, nullptr);
  }

  ~EventLoop() override { stop(); }

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  void post(Task task) override {
    AcquireSRWLockExclusive(&lock_);
    queue_.push_back(std::move(task));
    WakeConditionVariable(&cond_);
    ReleaseSRWLockExclusive(&lock_);
  }

  TimerId setTimeout(Task task, std::chrono::milliseconds delay) override {
    AcquireSRWLockExclusive(&lock_);
    TimerId id = nextTimerId_++;
    timers_.emplace(nowNs() + delay.count() * 1000000LL, TimerEntry{id, std::move(task)});
    WakeConditionVariable(&cond_);
    ReleaseSRWLockExclusive(&lock_);
    return id;
  }

  void clearTimeout(TimerId id) override {
    AcquireSRWLockExclusive(&lock_);
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
      if (it->second.id == id) {
        timers_.erase(it);
        break;
      }
    }
    ReleaseSRWLockExclusive(&lock_);
  }

  // Idempotent. Blocks until the loop thread has drained queued tasks and
  // exited. Pending timers are discarded, not fired.
  void stop() {
    AcquireSRWLockExclusive(&lock_);
    if (!stopping_) {
      stopping_ = true;
      WakeConditionVariable(&cond_);
    }
    ReleaseSRWLockExclusive(&lock_);
    if (thread_ != nullptr) {
      ::WaitForSingleObject(thread_, INFINITE);
      ::CloseHandle(thread_);
      thread_ = nullptr;
    }
  }

 private:
  struct TimerEntry {
    TimerId id;
    Task task;
  };

  // QueryPerformanceCounter, not GetTickCount64: tick granularity (~16ms)
  // would let timers fire EARLY relative to the requested delay, breaking
  // "at least" timer semantics. Deadlines are tracked in nanoseconds.
  static long long nowNs() {
    static const long long freq = [] {
      LARGE_INTEGER f;
      ::QueryPerformanceFrequency(&f);
      return static_cast<long long>(f.QuadPart);
    }();
    LARGE_INTEGER counter;
    ::QueryPerformanceCounter(&counter);
    const long long ticks = static_cast<long long>(counter.QuadPart);
    // split to avoid overflow: seconds part + remainder scaled to ns
    return (ticks / freq) * 1000000000LL + (ticks % freq) * 1000000000LL / freq;
  }

  static DWORD WINAPI trampoline(LPVOID self) {
    static_cast<EventLoop*>(self)->run();
    return 0;
  }

  void runOneUnlocked(Task& task) {
    ReleaseSRWLockExclusive(&lock_);
    try {
      task();
    } catch (...) {
      // Actor tasks catch internally; this backstop keeps the loop alive
      // for raw tasks posted directly by adapter users.
    }
    AcquireSRWLockExclusive(&lock_);
  }

  void run() {
    AcquireSRWLockExclusive(&lock_);
    while (!stopping_) {
      if (!queue_.empty()) {
        Task task = std::move(queue_.front());
        queue_.pop_front();
        runOneUnlocked(task);
        continue;
      }
      const long long now = nowNs();
      auto first = timers_.begin();
      if (first != timers_.end() && first->first <= now) {
        Task task = std::move(first->second.task);
        timers_.erase(first);
        runOneUnlocked(task);
        continue;
      }
      DWORD waitMs = INFINITE;
      if (first != timers_.end()) {
        const long long deltaNs = first->first - now;
        // round UP to whole ms so the wait never undershoots the deadline
        waitMs = deltaNs > 0 ? static_cast<DWORD>((deltaNs + 999999) / 1000000) : 0;
      }
      ::SleepConditionVariableSRW(&cond_, &lock_, waitMs, 0);
    }
    // drain tasks that were already queued (they may enqueue more; run those too)
    while (!queue_.empty()) {
      Task task = std::move(queue_.front());
      queue_.pop_front();
      runOneUnlocked(task);
    }
    timers_.clear();
    ReleaseSRWLockExclusive(&lock_);
  }

  HANDLE thread_ = nullptr;
  SRWLOCK lock_{};
  CONDITION_VARIABLE cond_{};
  bool stopping_ = false;
  std::deque<Task> queue_;
  std::multimap<long long, TimerEntry> timers_;  // deadline (QPC ns) -> entry
  TimerId nextTimerId_ = 1;
};

}  // namespace win32
}  // namespace xstate
