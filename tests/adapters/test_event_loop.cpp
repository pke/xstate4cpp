// Adapter tests: the only tests with real threads and real time. The same
// suite runs against posix::EventLoop on unix and win32::EventLoop on Windows.
#include "doctest.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <xstate/system.hpp>
#include <xstate/machine/machine.hpp>

#ifndef _WIN32
#include <xstate/adapters/posix/event_loop.hpp>
namespace loopns = xstate::posix;
#else
#include <xstate/adapters/win32/event_loop.hpp>
namespace loopns = xstate::win32;
#endif

using namespace xstate;
using ms = std::chrono::milliseconds;

namespace {
void spinUntil(const std::function<bool()>& pred, int timeoutMs = 5000) {
  auto start = std::chrono::steady_clock::now();
  while (!pred() && std::chrono::steady_clock::now() - start < ms(timeoutMs))
    std::this_thread::yield();
}
struct Ctx { int n = 0; };
}  // namespace

TEST_CASE("event loop: tasks run FIFO off-thread") {
  loopns::EventLoop loop;
  std::vector<int> order;
  std::atomic<bool> done{false};
  loop.post([&] { order.push_back(1); });
  loop.post([&] {
    order.push_back(2);
    done = true;
  });
  spinUntil([&] { return done.load(); });
  REQUIRE(done.load());
  CHECK(order == std::vector<int>{1, 2});
}

TEST_CASE("event loop: setTimeout fires once after >= delay; clearTimeout prevents firing") {
  loopns::EventLoop loop;
  std::atomic<int> fired{0};
  std::atomic<long long> elapsed{-1};
  auto t0 = std::chrono::steady_clock::now();
  loop.setTimeout(
      [&] {
        elapsed = std::chrono::duration_cast<ms>(std::chrono::steady_clock::now() - t0).count();
        fired++;
      },
      ms(50));
  auto cancelled = loop.setTimeout([&] { fired += 100; }, ms(50));
  loop.clearTimeout(cancelled);
  spinUntil([&] { return fired.load() >= 1; });
  std::this_thread::sleep_for(ms(80));  // give the cancelled timer a chance to misfire
  CHECK(fired.load() == 1);
  CHECK(elapsed.load() >= 50);
}

TEST_CASE("event loop: full actor system with after-transition and cross-thread send") {
  loopns::EventLoop loop;
  {
    MachineConfig<Ctx> c;
    c.initial = "a";
    c.states["a"].after[30] = "b";
    c.states["b"].on["POKE"] =
        transition<Ctx>("c").act(assign<Ctx>([](Ctx& x, const Event&) { x.n = 7; }));
    c.states["c"];
    SystemOptions o;
    o.executor = &loop;
    o.clock = &loop;
    auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c), o);
    sys->root()->start();
    spinUntil([&] {
      auto s = machineSnapshot<Ctx>(sys->root()->getSnapshot());
      return s != nullptr && s->matches("b");
    });
    REQUIRE(machineSnapshot<Ctx>(sys->root()->getSnapshot())->matches("b"));

    // send + getSnapshot from a foreign thread
    std::thread outsider([&] { sys->root()->send({"POKE"}); });
    outsider.join();
    spinUntil([&] {
      auto s = machineSnapshot<Ctx>(sys->root()->getSnapshot());
      return s != nullptr && s->matches("c");
    });
    auto snap = machineSnapshot<Ctx>(sys->root()->getSnapshot());
    REQUIRE(snap != nullptr);
    CHECK(snap->matches("c"));
    CHECK(snap->context.n == 7);
    sys->stop();
    spinUntil([&] { return sys->root()->getSnapshot()->status == SnapshotStatus::Stopped; });
    CHECK(sys->root()->getSnapshot()->status == SnapshotStatus::Stopped);
  }
}  // loop dtor joins here — the test hanging means shutdown is broken
