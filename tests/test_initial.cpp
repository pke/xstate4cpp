#include "doctest.h"
#include <xstate/machine/machine.hpp>
#include <xstate/snapshot.hpp>
using namespace xstate;

namespace {
struct Ctx {
  int entered = 0;
  std::vector<std::string> order{};
};
}  // namespace

TEST_CASE("initial snapshot of nested + parallel machine") {
  MachineConfig<Ctx> c;
  c.id = "m";
  c.initial = "p";
  c.states["p"].type = StateType::Parallel;
  c.states["p"].states["io"].initial = "reading";
  c.states["p"].states["io"].states["reading"].tags = {"busy"};
  c.states["p"].states["ui"].initial = "prompt";
  c.states["p"].states["ui"].states["prompt"];
  auto m = createMachine<Ctx>(c);
  ActorScope scope;
  auto snap = machineSnapshot<Ctx>(m->getInitialSnapshot(scope, {}));
  REQUIRE(snap != nullptr);
  CHECK(snap->matches("p.io.reading"));
  CHECK(snap->matches("p.ui.prompt"));
  CHECK(snap->hasTag("busy"));
  CHECK(snap->status == SnapshotStatus::Active);
}

TEST_CASE("entry actions run parents-first with mutable draft context") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.entry.push_back(assign<Ctx>([](Ctx& x, const Event&) { x.order.push_back("root"); }));
  c.states["a"].initial = "b";
  c.states["a"].entry.push_back(assign<Ctx>([](Ctx& x, const Event&) { x.order.push_back("a"); }));
  c.states["a"].states["b"].entry.push_back(
      assign<Ctx>([](Ctx& x, const Event&) { x.order.push_back("b"); }));
  auto m = createMachine<Ctx>(c);
  ActorScope scope;
  auto snap = machineSnapshot<Ctx>(m->getInitialSnapshot(scope, {}));
  CHECK(snap->context.order == std::vector<std::string>{"root", "a", "b"});
}

TEST_CASE("named actions from options execute at entry") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].entry.push_back(action<Ctx>("count"));
  MachineOptions<Ctx> o;
  o.actions["count"] = [](Ctx& x, const Event&) { x.entered++; };
  auto m = createMachine<Ctx>(c, o);
  ActorScope scope;
  CHECK(machineSnapshot<Ctx>(m->getInitialSnapshot(scope, {}))->context.entered == 1);
}
