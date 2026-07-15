#include "doctest.h"
#include <xstate/machine/machine.hpp>
using namespace xstate;

namespace {
struct Ctx { int n = 0; };
}

TEST_CASE("always transition chains until stable within one macrostep") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].on["GO"] =
      transition<Ctx>("b").act(assign<Ctx>([](Ctx& x, const Event&) { x.n = 5; }));
  c.states["b"].always.push_back(transition<Ctx>("c").guarded(
      guardFn<Ctx>([](const Ctx& x, const Event&) { return x.n >= 5; })));
  c.states["c"].always.push_back(transition<Ctx>("d").guarded(
      guardFn<Ctx>([](const Ctx& x, const Event&) { return x.n >= 5; })));
  c.states["d"];
  auto m = createMachine<Ctx>(c);
  ActorScope s;
  auto s1 = machineSnapshot<Ctx>(m->transition(m->getInitialSnapshot(s, {}), Event{"GO"}, s));
  CHECK(s1->matches("d"));  // b -> c -> d in ONE transition() call
}

TEST_CASE("raise queues an internal event processed in the same macrostep") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].on["GO"] = transition<Ctx>("b").act(raise<Ctx>(Event{"INTERNAL"}));
  c.states["b"].on["INTERNAL"] = "c";
  c.states["c"];
  auto m = createMachine<Ctx>(c);
  ActorScope s;
  CHECK(machineSnapshot<Ctx>(m->transition(m->getInitialSnapshot(s, {}), Event{"GO"}, s))
            ->matches("c"));
}

TEST_CASE("always at initial state settles during getInitialSnapshot") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].always.push_back(transition<Ctx>("b"));
  c.states["b"];
  auto m = createMachine<Ctx>(c);
  ActorScope s;
  CHECK(machineSnapshot<Ctx>(m->getInitialSnapshot(s, {}))->matches("b"));
}

TEST_CASE("unguarded always self-loop is detected") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].always.push_back(transition<Ctx>("b"));
  c.states["b"].always.push_back(transition<Ctx>("a"));
  auto m = createMachine<Ctx>(c);
  ActorScope s;
  CHECK_THROWS_AS(m->getInitialSnapshot(s, {}), ConfigError);
}
