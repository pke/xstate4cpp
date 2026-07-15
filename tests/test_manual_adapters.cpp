#include "doctest.h"
#include <xstate/adapters/manual/manual_executor.hpp>
#include <xstate/adapters/manual/test_clock.hpp>
using namespace xstate;

TEST_CASE("ManualExecutor runs tasks FIFO including re-entrant posts") {
  manual::ManualExecutor ex;
  std::vector<int> order;
  ex.post([&] {
    order.push_back(1);
    ex.post([&] { order.push_back(3); });
  });
  ex.post([&] { order.push_back(2); });
  CHECK(ex.pump() == 3);
  CHECK(order == std::vector<int>{1, 2, 3});
  CHECK(ex.pendingCount() == 0);
}

TEST_CASE("TestClock fires timers on advance, honors cancellation and ordering") {
  manual::TestClock clk;
  std::vector<int> fired;
  clk.setTimeout([&] { fired.push_back(100); }, std::chrono::milliseconds(100));
  auto t2 = clk.setTimeout([&] { fired.push_back(50); }, std::chrono::milliseconds(50));
  clk.setTimeout([&] { fired.push_back(70); }, std::chrono::milliseconds(70));
  clk.clearTimeout(t2);
  clk.advance(std::chrono::milliseconds(80));
  CHECK(fired == std::vector<int>{70});
  clk.advance(std::chrono::milliseconds(20));
  CHECK(fired == std::vector<int>{70, 100});
  CHECK(clk.pendingTimers() == 0);
}

TEST_CASE("advance is cumulative and timers set during advance are relative to virtual now") {
  manual::TestClock clk;
  std::vector<int> fired;
  clk.setTimeout(
      [&] {
        clk.setTimeout([&] { fired.push_back(2); }, std::chrono::milliseconds(10));
        fired.push_back(1);
      },
      std::chrono::milliseconds(10));
  clk.advance(std::chrono::milliseconds(10));
  CHECK(fired == std::vector<int>{1});
  clk.advance(std::chrono::milliseconds(10));
  CHECK(fired == std::vector<int>{1, 2});
}
