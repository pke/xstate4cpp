#pragma once

#if !defined(__unix__) && !defined(__APPLE__)
#error "xstate/adapters/posix/event_loop.hpp requires a POSIX system"
#endif

#include <pthread.h>
#include <time.h>

#include <deque>
#include <map>
#include <utility>

#include "../../interfaces/clock.hpp"
#include "../../interfaces/executor.hpp"

namespace xstate {
namespace posix {

// One pthread running a combined task queue + timer wheel. Implements both
// Executor and Clock so a whole ActorSystem runs on this single thread —
// which is what makes the core's lock-free design sound. post/setTimeout/
// clearTimeout are thread-safe. The destructor stops the loop, drains
// already-queued tasks, discards pending timers, and joins.
class EventLoop : public Executor, public Clock {
 public:
  EventLoop() {
    pthread_mutex_init(&mutex_, nullptr);
#if defined(__APPLE__)
    pthread_cond_init(&cond_, nullptr);  // waits use _relative_np on macOS
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&cond_, &attr);
    pthread_condattr_destroy(&attr);
#endif
    pthread_create(&thread_, nullptr, &EventLoop::trampoline, this);
  }

  ~EventLoop() override { stop(); }

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  void post(Task task) override {
    pthread_mutex_lock(&mutex_);
    queue_.push_back(std::move(task));
    pthread_cond_signal(&cond_);
    pthread_mutex_unlock(&mutex_);
  }

  TimerId setTimeout(Task task, std::chrono::milliseconds delay) override {
    pthread_mutex_lock(&mutex_);
    TimerId id = nextTimerId_++;
    timers_.emplace(nowNs() + delay.count() * 1000000LL, TimerEntry{id, std::move(task)});
    pthread_cond_signal(&cond_);
    pthread_mutex_unlock(&mutex_);
    return id;
  }

  void clearTimeout(TimerId id) override {
    pthread_mutex_lock(&mutex_);
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
      if (it->second.id == id) {
        timers_.erase(it);
        break;
      }
    }
    pthread_mutex_unlock(&mutex_);
  }

  // Idempotent. Blocks until the loop thread has drained queued tasks and
  // exited. Pending timers are discarded, not fired.
  void stop() {
    pthread_mutex_lock(&mutex_);
    if (stopping_) {
      pthread_mutex_unlock(&mutex_);
    } else {
      stopping_ = true;
      pthread_cond_signal(&cond_);
      pthread_mutex_unlock(&mutex_);
    }
    if (!joined_) {
      pthread_join(thread_, nullptr);
      joined_ = true;
    }
  }

 private:
  struct TimerEntry {
    TimerId id;
    Task task;
  };

  // Nanosecond deadlines: millisecond truncation would let timers fire up to
  // 1ms EARLY relative to the requested delay, which breaks "at least" timer
  // semantics (protocol timeouts must never fire before their deadline).
  static long long nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
  }

  static void* trampoline(void* self) {
    static_cast<EventLoop*>(self)->run();
    return nullptr;
  }

  void runOneUnlocked(Task& task) {
    pthread_mutex_unlock(&mutex_);
    try {
      task();
    } catch (...) {
      // Actor tasks catch internally; this backstop keeps the loop alive
      // for raw tasks posted directly by adapter users.
    }
    pthread_mutex_lock(&mutex_);
  }

  void run() {
    pthread_mutex_lock(&mutex_);
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
      if (first == timers_.end()) {
        pthread_cond_wait(&cond_, &mutex_);
      } else {
        const long long waitNs = first->first - now;
#if defined(__APPLE__)
        timespec rel;
        rel.tv_sec = waitNs / 1000000000LL;
        rel.tv_nsec = waitNs % 1000000000LL;
        pthread_cond_timedwait_relative_np(&cond_, &mutex_, &rel);
#else
        timespec abs;
        clock_gettime(CLOCK_MONOTONIC, &abs);
        abs.tv_sec += waitNs / 1000000000LL;
        abs.tv_nsec += waitNs % 1000000000LL;
        if (abs.tv_nsec >= 1000000000) {
          abs.tv_sec += 1;
          abs.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&cond_, &mutex_, &abs);
#endif
      }
    }
    // drain tasks that were already queued (they may enqueue more; run those too)
    while (!queue_.empty()) {
      Task task = std::move(queue_.front());
      queue_.pop_front();
      runOneUnlocked(task);
    }
    timers_.clear();
    pthread_mutex_unlock(&mutex_);
  }

  pthread_t thread_{};
  pthread_mutex_t mutex_{};
  pthread_cond_t cond_{};
  bool stopping_ = false;
  bool joined_ = false;
  std::deque<Task> queue_;
  std::multimap<long long, TimerEntry> timers_;  // deadline (monotonic ns) -> entry
  TimerId nextTimerId_ = 1;
};

}  // namespace posix
}  // namespace xstate
