#include "doctest.h"
#include <xstate/actor.hpp>
#include <xstate/system.hpp>
#include <xstate/machine/machine.hpp>
#include <xstate/adapters/manual/manual_executor.hpp>
#include <xstate/adapters/manual/test_clock.hpp>
using namespace xstate;
using ms = std::chrono::milliseconds;

namespace {
struct Ctx { int pings = 0; };
}

TEST_CASE("after fires on clock advance, is cancelled by state exit") {
  MachineConfig<Ctx> c;
  c.initial = "green";
  c.states["green"].after[1000] = "yellow";
  c.states["green"].on["OVERRIDE"] = "red";
  c.states["yellow"];
  c.states["red"];
  manual::ManualExecutor ex;
  manual::TestClock clk;
  SystemOptions o;
  o.executor = &ex;
  o.clock = &clk;

  SUBCASE("fires") {
    auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c), o);
    sys->root()->start();
    ex.pump();
    clk.advance(ms(999));
    ex.pump();
    CHECK(machineSnapshot<Ctx>(sys->root()->getSnapshot())->matches("green"));
    clk.advance(ms(1));
    ex.pump();
    CHECK(machineSnapshot<Ctx>(sys->root()->getSnapshot())->matches("yellow"));
  }
  SUBCASE("cancelled on exit") {
    auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c), o);
    sys->root()->start();
    ex.pump();
    sys->root()->send({"OVERRIDE"});
    ex.pump();
    CHECK(clk.pendingTimers() == 0);  // timer cleared
    clk.advance(ms(2000));
    ex.pump();
    CHECK(machineSnapshot<Ctx>(sys->root()->getSnapshot())->matches("red"));
  }
}

TEST_CASE("named delay resolved from options as fn of context") {
  MachineConfig<Ctx> c;
  c.initial = "wait";
  c.context.pings = 3;
  c.states["wait"].after["BACKOFF"] = "done";
  c.states["done"];
  MachineOptions<Ctx> mo;
  mo.delays["BACKOFF"] = std::function<long long(const Ctx&, const Event&)>(
      [](const Ctx& x, const Event&) { return 100LL * x.pings; });
  manual::ManualExecutor ex;
  manual::TestClock clk;
  SystemOptions o;
  o.executor = &ex;
  o.clock = &clk;
  auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c, mo), o);
  sys->root()->start();
  ex.pump();
  clk.advance(ms(299));
  ex.pump();
  CHECK(machineSnapshot<Ctx>(sys->root()->getSnapshot())->matches("wait"));
  clk.advance(ms(1));
  ex.pump();
  CHECK(machineSnapshot<Ctx>(sys->root()->getSnapshot())->matches("done"));
}

TEST_CASE("delayed raise with cancel action") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].entry.push_back(raise<Ctx>(Event{"LATER"}, 500, "my-send"));
  c.states["a"].on["LATER"] = "b";
  c.states["a"].on["ABORT"] = transition<Ctx>().act(cancel<Ctx>("my-send"));
  c.states["b"];
  manual::ManualExecutor ex;
  manual::TestClock clk;
  SystemOptions o;
  o.executor = &ex;
  o.clock = &clk;

  SUBCASE("fires when not cancelled") {
    auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c), o);
    sys->root()->start();
    ex.pump();
    clk.advance(ms(500));
    ex.pump();
    CHECK(machineSnapshot<Ctx>(sys->root()->getSnapshot())->matches("b"));
  }
  SUBCASE("cancelled") {
    auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c), o);
    sys->root()->start();
    ex.pump();
    sys->root()->send({"ABORT"});
    ex.pump();
    clk.advance(ms(1000));
    ex.pump();
    CHECK(machineSnapshot<Ctx>(sys->root()->getSnapshot())->matches("a"));
  }
}
