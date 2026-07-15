// Ported from xstate v5's own test suite (packages/core/test/): guards.test.ts,
// actions.test.ts, transient.test.ts. Each case cites its upstream test title.
#include "doctest.h"
#include <xstate/machine/machine.hpp>
using namespace xstate;

namespace {
struct Ctx {
  int count = 0;
  std::vector<std::string> log{};
};

ActionRef<Ctx> mark(std::string s) {
  return assign<Ctx>([s = std::move(s)](Ctx& x, const Event&) { x.log.push_back(s); });
}

using Snap = std::shared_ptr<const MachineSnapshot<Ctx>>;

struct Rig {
  std::shared_ptr<Machine<Ctx>> m;
  ActorScope scope;
  SnapshotPtr snap;
  explicit Rig(std::shared_ptr<Machine<Ctx>> machine) : m(std::move(machine)) {
    snap = m->getInitialSnapshot(scope, {});
  }
  Snap send(Event e) {
    snap = m->transition(snap, std::move(e), scope);
    return machineSnapshot<Ctx>(snap);
  }
  Snap state() const { return machineSnapshot<Ctx>(snap); }
};
}  // namespace

// guards.test.ts: "should not transition if no condition is met"
TEST_CASE("upstream/guards: fully guarded-out event changes nothing") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].exit.push_back(mark("exit a"));
  c.states["a"].on["TIMER"]
      .add(transition<Ctx>("b").guarded(guardFn<Ctx>(
          [](const Ctx&, const Event& e) { return *e.dataAs<int>() > 200; })))
      .add(transition<Ctx>("c").guarded(guardFn<Ctx>(
          [](const Ctx&, const Event& e) { return *e.dataAs<int>() > 100; })));
  c.states["b"].entry.push_back(mark("enter b"));
  c.states["c"].entry.push_back(mark("enter c"));
  Rig r(createMachine<Ctx>(c));
  auto before = r.snap;
  auto s = r.send(Event{"TIMER", 10});
  CHECK(s->matches("a"));
  CHECK(r.snap == before);  // no transition at all
  CHECK(s->context.log.empty());
}

// guards.test.ts: "should allow a matching transition" (+ "should guard against transition")
TEST_CASE("upstream/guards: stateIn guard across parallel regions") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  c.states["A"].initial = "A2";
  c.states["A"].states["A0"];
  c.states["A"].states["A2"];
  c.states["B"].initial = "B0";
  c.states["B"].states["B0"].always.push_back(
      transition<Ctx>("B4").guarded(guardFn<Ctx>([](const Ctx&, const Event&) { return false; })));
  c.states["B"].states["B0"].on["T1"] = transition<Ctx>("B1").guarded(
      guardFn<Ctx>([](const Ctx&, const Event&) { return false; }));
  c.states["B"].states["B0"].on["T2"] = transition<Ctx>("B2").guarded(stateIn<Ctx>("A.A2"));
  c.states["B"].states["B1"];
  c.states["B"].states["B2"];
  c.states["B"].states["B4"];

  SUBCASE("matching stateIn allows the transition") {
    Rig r(createMachine<Ctx>(c));
    auto s = r.send({"T2"});
    CHECK(s->matches("A.A2"));
    CHECK(s->matches("B.B2"));
  }
  SUBCASE("failing guard blocks the transition") {
    Rig r(createMachine<Ctx>(c));
    auto s = r.send({"T1"});
    CHECK(s->matches("B.B0"));
  }
}

// guards.test.ts: "should check guards with interim states"
TEST_CASE("upstream/guards: stateIn sees interim microstep configurations") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  c.states["A"].initial = "A2";
  c.states["A"].states["A2"].on["A"] = "A3";
  c.states["A"].states["A3"].always.push_back(transition<Ctx>("A4"));
  c.states["A"].states["A4"].always.push_back(transition<Ctx>("A5"));
  c.states["A"].states["A5"];
  c.states["B"].initial = "B0";
  c.states["B"].states["B0"].always.push_back(
      transition<Ctx>("B4").guarded(stateIn<Ctx>("A.A4")));
  c.states["B"].states["B4"];
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"A"});
  CHECK(s->matches("A.A5"));
  CHECK(s->matches("B.B4"));  // fired while A was momentarily in A4
}

// guards.test.ts: not() "should guard with nested built-in guards"
TEST_CASE("upstream/guards: nested not/and composition resolves named guards") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].on["EVENT"] = transition<Ctx>("b").guarded(
      not_<Ctx>(and_<Ctx>({not_<Ctx>("truthy"), guardNamed<Ctx>("truthy")})));
  c.states["b"];
  MachineOptions<Ctx> o;
  o.guards["truthy"] = [](const Ctx&, const Event&) { return true; };
  o.guards["falsy"] = [](const Ctx&, const Event&) { return false; };
  Rig r(createMachine<Ctx>(c, o));
  CHECK(r.send({"EVENT"})->matches("b"));
}

// actions.test.ts: "should return actions for parallel machines"
TEST_CASE("upstream/actions: parallel transition action order") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  auto& a = c.states["a"];
  a.initial = "a1";
  a.entry.push_back(mark("enter_a"));
  a.exit.push_back(mark("exit_a"));
  a.states["a1"].entry.push_back(mark("enter_a1"));
  a.states["a1"].exit.push_back(mark("exit_a1"));
  a.states["a1"].on["CHANGE"] =
      transition<Ctx>("a2").act(mark("do_a2")).act(mark("another_do_a2"));
  a.states["a2"].entry.push_back(mark("enter_a2"));
  a.states["a2"].exit.push_back(mark("exit_a2"));
  auto& b = c.states["b"];
  b.initial = "b1";
  b.entry.push_back(mark("enter_b"));
  b.exit.push_back(mark("exit_b"));
  b.states["b1"].entry.push_back(mark("enter_b1"));
  b.states["b1"].exit.push_back(mark("exit_b1"));
  b.states["b1"].on["CHANGE"] = transition<Ctx>("b2").act(mark("do_b2"));
  b.states["b2"].entry.push_back(mark("enter_b2"));
  b.states["b2"].exit.push_back(mark("exit_b2"));
  Rig r(createMachine<Ctx>(c));
  auto afterStart = r.state()->context.log;  // discard start entries
  auto s = r.send({"CHANGE"});
  std::vector<std::string> delta(s->context.log.begin() + afterStart.size(),
                                 s->context.log.end());
  CHECK(delta == std::vector<std::string>{"exit_b1", "exit_a1", "do_a2", "another_do_a2",
                                          "do_b2", "enter_a2", "enter_b2"});
}

// actions.test.ts: "should return nested actions in the correct (child to parent) order"
TEST_CASE("upstream/actions: nested exit child-to-parent, enter parent-to-child") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].initial = "a1";
  c.states["a"].exit.push_back(mark("exit: a"));
  c.states["a"].states["a1"].exit.push_back(mark("exit: a.a1"));
  c.states["a"].on["CHANGE"] = "b";
  c.states["b"].initial = "b1";
  c.states["b"].entry.push_back(mark("enter: b"));
  c.states["b"].states["b1"].entry.push_back(mark("enter: b.b1"));
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"CHANGE"});
  CHECK(s->context.log ==
        std::vector<std::string>{"exit: a.a1", "exit: a", "enter: b", "enter: b.b1"});
}

// actions.test.ts: "should ignore parent state actions for same-parent substates"
TEST_CASE("upstream/actions: same-parent sibling transition keeps parent") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].initial = "a1";
  c.states["a"].entry.push_back(mark("enter: a"));
  c.states["a"].exit.push_back(mark("exit: a"));
  c.states["a"].states["a1"].exit.push_back(mark("exit: a.a1"));
  c.states["a"].states["a1"].on["NEXT"] = "a2";
  c.states["a"].states["a2"].entry.push_back(mark("enter: a.a2"));
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"NEXT"});
  CHECK(s->context.log ==
        std::vector<std::string>{"enter: a", "exit: a.a1", "enter: a.a2"});
}

// actions.test.ts: "should exit children of parallel state nodes"
TEST_CASE("upstream/actions: exiting a parallel exits regions in reverse doc order") {
  MachineConfig<Ctx> c;
  c.initial = "B";
  c.states["A"].entry.push_back(mark("enter: A"));
  auto& B = c.states["B"];
  B.type = StateType::Parallel;
  B.exit.push_back(mark("exit: B"));
  B.on["to-A"] = "A";
  B.states["C"].initial = "C1";
  B.states["C"].exit.push_back(mark("exit: B.C"));
  B.states["C"].states["C1"].exit.push_back(mark("exit: B.C.C1"));
  B.states["D"].initial = "D1";
  B.states["D"].exit.push_back(mark("exit: B.D"));
  B.states["D"].states["D1"].exit.push_back(mark("exit: B.D.D1"));
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"to-A"});
  CHECK(s->context.log ==
        std::vector<std::string>{"exit: B.D.D1", "exit: B.D", "exit: B.C.C1", "exit: B.C",
                                 "exit: B", "enter: A"});
}

// actions.test.ts: "should reenter targeted ancestor (as it's a descendant of the transition domain)"
TEST_CASE("upstream/actions: targeting an ancestor exits and re-enters it") {
  MachineConfig<Ctx> c;
  c.initial = "loaded";
  auto& loaded = c.states["loaded"];
  loaded.id = "loaded";
  loaded.initial = "idle";
  loaded.entry.push_back(mark("enter: loaded"));
  loaded.exit.push_back(mark("exit: loaded"));
  loaded.states["idle"].entry.push_back(mark("enter: loaded.idle"));
  loaded.states["idle"].exit.push_back(mark("exit: loaded.idle"));
  loaded.states["idle"].on["UPDATE"] = "#loaded";
  Rig r(createMachine<Ctx>(c));
  auto afterStart = r.state()->context.log;
  auto s = r.send({"UPDATE"});
  std::vector<std::string> delta(s->context.log.begin() + afterStart.size(),
                                 s->context.log.end());
  CHECK(delta == std::vector<std::string>{"exit: loaded.idle", "exit: loaded",
                                          "enter: loaded", "enter: loaded.idle"});
}

// actions.test.ts: "should exit deep descendant during a default self-transition"
// + "should not reenter leaf state during its default self-transition"
// + "should exit deep descendant during a reentering self-transition"
TEST_CASE("upstream/actions: self-transition semantics") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  auto& a = c.states["a"];
  a.initial = "a1";
  a.entry.push_back(mark("enter: a"));
  a.exit.push_back(mark("exit: a"));
  a.states["a1"].initial = "a11";
  a.states["a1"].entry.push_back(mark("enter: a.a1"));
  a.states["a1"].exit.push_back(mark("exit: a.a1"));
  a.states["a1"].states["a11"].entry.push_back(mark("enter: a.a1.a11"));
  a.states["a1"].states["a11"].exit.push_back(mark("exit: a.a1.a11"));

  SUBCASE("default self-transition on compound exits descendants only") {
    auto cc = c;
    cc.states["a"].on["EV"] = "a";
    Rig r(createMachine<Ctx>(cc));
    auto afterStart = r.state()->context.log;
    auto s = r.send({"EV"});
    std::vector<std::string> delta(s->context.log.begin() + afterStart.size(),
                                   s->context.log.end());
    CHECK(delta == std::vector<std::string>{"exit: a.a1.a11", "exit: a.a1", "enter: a.a1",
                                            "enter: a.a1.a11"});
  }
  SUBCASE("reentering self-transition also exits the state itself") {
    auto cc = c;
    auto t = transition<Ctx>("a");
    t.reenter = true;
    cc.states["a"].on["EV"] = t;
    Rig r(createMachine<Ctx>(cc));
    auto afterStart = r.state()->context.log;
    auto s = r.send({"EV"});
    std::vector<std::string> delta(s->context.log.begin() + afterStart.size(),
                                   s->context.log.end());
    CHECK(delta == std::vector<std::string>{"exit: a.a1.a11", "exit: a.a1", "exit: a",
                                            "enter: a", "enter: a.a1", "enter: a.a1.a11"});
  }
  SUBCASE("default self-transition on a leaf does nothing") {
    auto cc = c;
    cc.states["a"].states["a1"].states["a11"].on["EV"] = "a11";
    Rig r(createMachine<Ctx>(cc));
    auto afterStart = r.state()->context.log;
    auto s = r.send({"EV"});
    CHECK(s->context.log.size() == afterStart.size());
  }
}

// transient.test.ts: "should carry actions from previous transitions within same step"
TEST_CASE("upstream/eventless: pass-through transient state carries actions") {
  MachineConfig<Ctx> c;
  c.initial = "A";
  c.states["A"].exit.push_back(mark("exit_A"));
  c.states["A"].on["TIMER"] = transition<Ctx>("T").act(mark("timer"));
  c.states["T"].always.push_back(transition<Ctx>("B"));
  c.states["B"].entry.push_back(mark("enter_B"));
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"TIMER"});
  CHECK(s->matches("B"));
  CHECK(s->context.log == std::vector<std::string>{"exit_A", "timer", "enter_B"});
}

// transient.test.ts: "should select eventless transition before processing raised events"
TEST_CASE("upstream/eventless: always runs before raised events") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].on["FOO"] = "b";
  c.states["b"].entry.push_back(raise<Ctx>(Event{"BAR"}));
  c.states["b"].always.push_back(transition<Ctx>("c"));
  c.states["b"].on["BAR"] = "d";
  c.states["c"].on["BAR"] = "e";
  c.states["d"];
  c.states["e"];
  Rig r(createMachine<Ctx>(c));
  CHECK(r.send({"FOO"})->matches("e"));
}

// transient.test.ts: "should not select wildcard for eventless transition"
TEST_CASE("upstream/eventless: wildcard never matches the eventless event") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].on["FOO"] = "b";
  c.states["b"].always.push_back(transition<Ctx>("pass"));
  c.states["b"].on["*"] = "fail";
  c.states["fail"];
  c.states["pass"];
  Rig r(createMachine<Ctx>(c));
  CHECK(r.send({"FOO"})->matches("pass"));
}

// transient.test.ts: "should execute all eventless transitions in the same microstep"
TEST_CASE("upstream/eventless: cross-region eventless cascade in one macrostep") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  auto& A = c.states["A"];
  A.initial = "A1";
  A.states["A1"].on["E"] = "A2";
  A.states["A2"].always.push_back(transition<Ctx>("A3"));
  A.states["A3"].always.push_back(transition<Ctx>("A4").guarded(stateIn<Ctx>("B.B3")));
  A.states["A4"];
  auto& B = c.states["B"];
  B.initial = "B1";
  B.states["B1"].on["E"] = "B2";
  B.states["B2"].always.push_back(transition<Ctx>("B3").guarded(stateIn<Ctx>("A.A2")));
  B.states["B3"].always.push_back(transition<Ctx>("B4").guarded(stateIn<Ctx>("A.A3")));
  B.states["B4"];
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"E"});
  CHECK(s->matches("A.A4"));
  CHECK(s->matches("B.B4"));
}

// transient.test.ts: "should loop (but not infinitely) for assign actions"
// + "should avoid infinite loops with eventless transitions"
TEST_CASE("upstream/eventless: targetless always with assign loops until guard fails") {
  MachineConfig<Ctx> c;
  c.initial = "counting";
  c.states["counting"].always.push_back(
      transition<Ctx>()
          .guarded(guardFn<Ctx>([](const Ctx& x, const Event&) { return x.count < 5; }))
          .act(assign<Ctx>([](Ctx& x, const Event&) { x.count++; })));
  Rig r(createMachine<Ctx>(c));
  CHECK(r.state()->context.count == 5);

  MachineConfig<Ctx> loop;
  loop.initial = "a";
  loop.states["a"].always.push_back(transition<Ctx>("b"));
  loop.states["b"].always.push_back(transition<Ctx>("c"));
  loop.states["c"].always.push_back(transition<Ctx>("a"));
  auto m = createMachine<Ctx>(loop);
  ActorScope scope;
  CHECK_THROWS_WITH_AS(m->getInitialSnapshot(scope, {}),
                       doctest::Contains("infinite loop"), ConfigError);
}

// transient.test.ts: "should execute an always transition after a raised transition
// even if that raised transition doesn't change the state"
TEST_CASE("upstream/eventless: always re-fires after each processed event") {
  std::vector<int> spyCalls;
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.always.push_back(transition<Ctx>().act(action<Ctx>("spy")));
  c.on["EV"] = transition<Ctx>().act(raise<Ctx>(Event{"RAISED"}));
  c.on["RAISED"] = transition<Ctx>().act(assign<Ctx>([](Ctx& x, const Event&) { x.count++; }));
  c.states["a"];
  MachineOptions<Ctx> o;
  o.actions["spy"] = [&spyCalls](Ctx& x, const Event&) { spyCalls.push_back(x.count); };
  Rig r(createMachine<Ctx>(c, o));
  spyCalls.clear();  // ignore the evaluation at start
  r.send({"EV"});
  CHECK(spyCalls == std::vector<int>{0, 1});
}
