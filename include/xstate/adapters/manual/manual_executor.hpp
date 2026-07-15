#pragma once

#include <cstddef>
#include <deque>
#include <utility>

#include "../../interfaces/executor.hpp"

namespace xstate {
namespace manual {

// Caller-pumped executor: tasks queue up until pump() runs them on the
// calling thread. NOT thread-safe by design — single-threaded use only
// (tests, UI loops that own their own thread).
class ManualExecutor : public Executor {
 public:
  void post(Task task) override { queue_.push_back(std::move(task)); }

  // Runs until the queue is empty, including tasks posted while pumping.
  // Returns the number of tasks executed.
  std::size_t pump() {
    std::size_t count = 0;
    while (!queue_.empty()) {
      Task task = std::move(queue_.front());
      queue_.pop_front();
      ++count;
      task();
    }
    return count;
  }

  std::size_t pendingCount() const { return queue_.size(); }

 private:
  std::deque<Task> queue_;
};

}  // namespace manual
}  // namespace xstate
