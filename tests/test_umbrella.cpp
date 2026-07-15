// Verifies the umbrella header alone is enough for typical usage.
#include "doctest.h"
#include <xstate/xstate.hpp>
#include <xstate/adapters/manual/manual_executor.hpp>
#include <xstate/adapters/manual/test_clock.hpp>
using namespace xstate;

namespace {
struct Ctx { int toggles = 0; };
}

TEST_CASE("umbrella header covers machine + actor + system usage") {
  MachineConfig<Ctx> c;
  c.initial = "inactive";
  c.states["inactive"].on["TOGGLE"] =
      transition<Ctx>("active").act(assign<Ctx>([](Ctx& x, const Event&) { x.toggles++; }));
  c.states["active"].on["TOGGLE"] = "inactive";
  manual::ManualExecutor ex;
  manual::TestClock clk;
  SystemOptions o;
  o.executor = &ex;
  o.clock = &clk;
  auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c), o);
  sys->root()->start();
  ex.pump();
  sys->root()->send({"TOGGLE"});
  ex.pump();
  auto snap = machineSnapshot<Ctx>(sys->root()->getSnapshot());
  CHECK(snap->matches("active"));
  CHECK(snap->context.toggles == 1);
}
