#include "doctest.h"
#include <xstate/machine/machine.hpp>
using namespace xstate;

namespace {
struct Ctx { int n = 0; };
}

TEST_CASE("parallel: one event can advance both regions; done when all regions final") {
  MachineConfig<Ctx> c;
  c.initial = "work";
  auto& p = c.states["work"];
  p.type = StateType::Parallel;
  p.id = "work";
  p.states["io"].initial = "run";
  p.states["io"].states["run"].on["FINISH"] = "done";
  p.states["io"].states["done"].type = StateType::Final;
  p.states["ui"].initial = "run";
  p.states["ui"].states["run"].on["FINISH"] = "done";
  p.states["ui"].states["done"].type = StateType::Final;
  p.onDone.push_back(transition<Ctx>("#end"));
  c.states["end"].id = "end";
  c.states["end"].type = StateType::Final;
  c.states["end"].output = std::any{std::string("bye")};
  auto m = createMachine<Ctx>(c);
  ActorScope s;
  auto s1 = machineSnapshot<Ctx>(m->transition(m->getInitialSnapshot(s, {}), Event{"FINISH"}, s));
  CHECK(s1->status == SnapshotStatus::Done);  // both regions finished on one event
  REQUIRE(s1->output.has_value());
  CHECK(*std::any_cast<std::string>(&s1->output) == "bye");
}

TEST_CASE("compound onDone fires when final child entered") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].initial = "x";
  c.states["a"].id = "a";
  c.states["a"].states["x"].on["END"] = "fin";
  c.states["a"].states["fin"].type = StateType::Final;
  c.states["a"].onDone.push_back(transition<Ctx>("b"));
  c.states["b"];
  auto m = createMachine<Ctx>(c);
  ActorScope s;
  CHECK(machineSnapshot<Ctx>(m->transition(m->getInitialSnapshot(s, {}), Event{"END"}, s))
            ->matches("b"));
}

TEST_CASE("events after Done are ignored") {
  MachineConfig<Ctx> c;
  c.initial = "fin";
  c.states["fin"].type = StateType::Final;
  auto m = createMachine<Ctx>(c);
  ActorScope s;
  auto s0 = m->getInitialSnapshot(s, {});
  CHECK(machineSnapshot<Ctx>(s0)->status == SnapshotStatus::Done);
  CHECK(m->transition(s0, Event{"ANY"}, s) == s0);
}
