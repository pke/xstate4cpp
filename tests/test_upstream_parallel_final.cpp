// Ported from xstate v5's own test suite: parallel.test.ts, final.test.ts,
// deterministic.test.ts. Each case cites its upstream test title.
#include "doctest.h"
#include <xstate/machine/machine.hpp>
using namespace xstate;

namespace {
struct Ctx {
  std::string value;
  std::vector<std::string> log{};
  std::string doneEventType;
  std::string revealedSecret;
  int count = 0;
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

// parallel.test.ts: "should properly transition according to entry events on an initial state"
TEST_CASE("upstream/parallel: raise in initial entry reaches sibling regions") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  c.states["OUTER1"].initial = "B";
  c.states["OUTER1"].states["A"];
  c.states["OUTER1"].states["B"].entry.push_back(raise<Ctx>(Event{"CLEAR"}));
  auto& o2 = c.states["OUTER2"];
  o2.type = StateType::Parallel;
  o2.states["INNER1"].initial = "ON";
  o2.states["INNER1"].states["OFF"];
  o2.states["INNER1"].states["ON"].on["CLEAR"] = "OFF";
  o2.states["INNER2"].initial = "OFF";
  o2.states["INNER2"].states["OFF"];
  o2.states["INNER2"].states["ON"];
  Rig r(createMachine<Ctx>(c));
  CHECK(r.state()->matches("OUTER1.B"));
  CHECK(r.state()->matches("OUTER2.INNER1.OFF"));
  CHECK(r.state()->matches("OUTER2.INNER2.OFF"));
}

// parallel.test.ts: "should properly transition when raising events for a parallel state"
TEST_CASE("upstream/parallel: raised-event cascade across regions") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  auto& o1 = c.states["OUTER1"];
  o1.initial = "C";
  o1.states["A"].entry.push_back(raise<Ctx>(Event{"TURN_OFF"}));
  o1.states["A"].on["EVENT_OUTER1_B"] = "B";
  o1.states["A"].on["EVENT_OUTER1_C"] = "C";
  o1.states["B"].entry.push_back(raise<Ctx>(Event{"TURN_ON"}));
  o1.states["B"].on["EVENT_OUTER1_A"] = "A";
  o1.states["B"].on["EVENT_OUTER1_C"] = "C";
  o1.states["C"].entry.push_back(raise<Ctx>(Event{"CLEAR"}));
  o1.states["C"].on["EVENT_OUTER1_A"] = "A";
  o1.states["C"].on["EVENT_OUTER1_B"] = "B";
  auto& o2 = c.states["OUTER2"];
  o2.type = StateType::Parallel;
  o2.states["INNER1"].initial = "ON";
  o2.states["INNER1"].states["OFF"].on["TURN_ON"] = "ON";
  o2.states["INNER1"].states["ON"].on["CLEAR"] = "OFF";
  o2.states["INNER2"].initial = "OFF";
  o2.states["INNER2"].states["OFF"].on["TURN_ON"] = "ON";
  o2.states["INNER2"].states["ON"].on["TURN_OFF"] = "OFF";
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"EVENT_OUTER1_B"});
  CHECK(s->matches("OUTER1.B"));
  CHECK(s->matches("OUTER2.INNER1.ON"));
  CHECK(s->matches("OUTER2.INNER2.ON"));
}

// parallel.test.ts: "should handle simultaneous orthogonal transitions"
TEST_CASE("upstream/parallel: one event fires in two orthogonal regions") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  c.states["editing"].on["CHANGE"] = transition<Ctx>().act(
      assign<Ctx>([](Ctx& x, const Event& e) { x.value = *e.dataAs<std::string>(); }));
  c.states["status"].initial = "unsaved";
  c.states["status"].states["unsaved"].on["SAVE"] = "saved";
  c.states["status"].states["saved"].on["CHANGE"] = "unsaved";
  Rig r(createMachine<Ctx>(c));
  r.send({"SAVE"});
  auto s = r.send({"CHANGE", std::string("something")});
  CHECK(s->matches("status.unsaved"));
  CHECK(s->context.value == "something");
}

// parallel.test.ts: "should raise a 'xstate.done.state.*' event when all child states
// reach final state" (single event finishing all regions)
TEST_CASE("upstream/parallel: one event finishes all regions and completes the machine") {
  MachineConfig<Ctx> c;
  c.initial = "p";
  auto& p = c.states["p"];
  p.type = StateType::Parallel;
  for (const char* key : {"a", "b", "c"}) {
    p.states[key].initial = "idle";
    p.states[key].states["idle"].on["FINISH"] = "finished";
    p.states[key].states["finished"].type = StateType::Final;
  }
  p.onDone.push_back(transition<Ctx>("success"));
  c.states["success"].type = StateType::Final;
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"FINISH"});
  CHECK(s->status == SnapshotStatus::Done);
}

// parallel.test.ts: "...when a pseudostate of a history type is directly on a parallel state"
TEST_CASE("upstream/parallel: history child does not block parallel onDone") {
  MachineConfig<Ctx> c;
  c.initial = "parallelSteps";
  auto& p = c.states["parallelSteps"];
  p.type = StateType::Parallel;
  p.states["hist"].type = StateType::History;
  p.states["one"].initial = "wait_one";
  p.states["one"].states["wait_one"].on["finish_one"] = "done";
  p.states["one"].states["done"].type = StateType::Final;
  p.states["two"].initial = "wait_two";
  p.states["two"].states["wait_two"].on["finish_two"] = "done";
  p.states["two"].states["done"].type = StateType::Final;
  p.onDone.push_back(transition<Ctx>("finished"));
  c.states["finished"];
  Rig r(createMachine<Ctx>(c));
  r.send({"finish_one"});
  CHECK(r.send({"finish_two"})->matches("finished"));
}

// parallel.test.ts: "source parallel region should be reentered when a transition
// within it targets another parallel region (parallel root)"
TEST_CASE("upstream/parallel: cross-region target re-enters the source region") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  auto& op = c.states["Operation"];
  op.initial = "Waiting";
  op.entry.push_back(mark("enter: Operation"));
  op.exit.push_back(mark("exit: Operation"));
  op.states["Waiting"].entry.push_back(mark("enter: Operation.Waiting"));
  op.states["Waiting"].exit.push_back(mark("exit: Operation.Waiting"));
  op.states["Waiting"].on["TOGGLE_MODE"] = "#Demo";
  op.states["Fetching"];
  auto& mode = c.states["Mode"];
  mode.initial = "Normal";
  mode.entry.push_back(mark("enter: Mode"));
  mode.exit.push_back(mark("exit: Mode"));
  mode.states["Normal"].entry.push_back(mark("enter: Mode.Normal"));
  mode.states["Normal"].exit.push_back(mark("exit: Mode.Normal"));
  mode.states["Demo"].id = "Demo";
  mode.states["Demo"].entry.push_back(mark("enter: Mode.Demo"));
  Rig r(createMachine<Ctx>(c));
  auto afterStart = r.state()->context.log;
  auto s = r.send({"TOGGLE_MODE"});
  std::vector<std::string> delta(s->context.log.begin() + afterStart.size(),
                                 s->context.log.end());
  CHECK(delta == std::vector<std::string>{
                     "exit: Mode.Normal", "exit: Mode", "exit: Operation.Waiting",
                     "exit: Operation", "enter: Operation", "enter: Operation.Waiting",
                     "enter: Mode", "enter: Mode.Demo"});
}

// parallel.test.ts: "targetless transition on a parallel state should not enter nor exit any states"
TEST_CASE("upstream/parallel: targetless transition exits/enters nothing") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  c.states["first"].initial = "disabled";
  c.states["first"].entry.push_back(mark("enter: first"));
  c.states["first"].exit.push_back(mark("exit: first"));
  c.states["first"].states["disabled"].entry.push_back(mark("enter: first.disabled"));
  c.states["first"].states["disabled"].exit.push_back(mark("exit: first.disabled"));
  c.states["first"].states["enabled"];
  c.states["second"].entry.push_back(mark("enter: second"));
  c.states["second"].exit.push_back(mark("exit: second"));
  c.on["MY_EVENT"] = transition<Ctx>();  // targetless, no actions
  Rig r(createMachine<Ctx>(c));
  auto afterStart = r.state()->context.log;
  auto s = r.send({"MY_EVENT"});
  CHECK(s->context.log.size() == afterStart.size());
}

// parallel.test.ts: "should calculate the entry set for reentering transitions in parallel states"
TEST_CASE("upstream/parallel: reentering self-transition inside a region re-runs entry") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  auto& foo = c.states["foo"];
  foo.initial = "foobar";
  foo.states["foobar"].on["GOTO_FOOBAZ"] = "foobaz";
  foo.states["foobaz"].entry.push_back(mark("entered foobaz"));
  auto t = transition<Ctx>("foobaz");
  t.reenter = true;
  foo.states["foobaz"].on["GOTO_FOOBAZ"] = t;
  c.states["bar"];
  Rig r(createMachine<Ctx>(c));
  r.send({"GOTO_FOOBAZ"});
  auto s = r.send({"GOTO_FOOBAZ"});
  CHECK(s->context.log.size() == 2);
}

// parallel.test.ts: "regions should be able to transition to orthogonal regions"
TEST_CASE("upstream/parallel: multi-target transition across orthogonal regions") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  c.states["Pages"].initial = "About";
  c.states["Pages"].states["About"].id = "About";
  c.states["Pages"].states["Dashboard"].id = "Dashboard";
  auto& menu = c.states["Menu"];
  menu.initial = "Closed";
  menu.states["Closed"].id = "Closed";
  menu.states["Closed"].on["toggle"] = "#Opened";
  menu.states["Opened"].id = "Opened";
  menu.states["Opened"].on["toggle"] = "#Closed";
  menu.states["Opened"].on["go to dashboard"] =
      transition<Ctx>("#Dashboard").target("#Opened");
  Rig r(createMachine<Ctx>(c));
  r.send({"toggle"});
  auto s = r.send({"go to dashboard"});
  CHECK(s->matches("Menu.Opened"));
  CHECK(s->matches("Pages.Dashboard"));
}

// final.test.ts: "should emit the 'xstate.done.state.*' event when all nested states
// are in their final states"
TEST_CASE("upstream/final: done event carries the v5 wire-format name") {
  MachineConfig<Ctx> c;
  c.id = "m";
  c.initial = "foo";
  auto& foo = c.states["foo"];
  foo.type = StateType::Parallel;
  foo.states["first"].initial = "a";
  foo.states["first"].states["a"].on["NEXT_1"] = "b";
  foo.states["first"].states["b"].type = StateType::Final;
  foo.states["second"].initial = "a";
  foo.states["second"].states["a"].on["NEXT_2"] = "b";
  foo.states["second"].states["b"].type = StateType::Final;
  foo.onDone.push_back(transition<Ctx>("bar").act(
      assign<Ctx>([](Ctx& x, const Event& e) { x.doneEventType = e.type; })));
  c.states["bar"];
  Rig r(createMachine<Ctx>(c));
  r.send({"NEXT_1"});
  auto s = r.send({"NEXT_2"});
  CHECK(s->matches("bar"));
  CHECK(s->context.doneEventType == "xstate.done.state.m.foo");
}

// final.test.ts: "should execute final child state actions first"
TEST_CASE("upstream/final: cascading onDone runs innermost-first") {
  MachineConfig<Ctx> c;
  c.initial = "foo";
  auto& foo = c.states["foo"];
  foo.initial = "bar";
  foo.onDone.push_back(transition<Ctx>().act(mark("fooAction")));
  foo.states["bar"].initial = "baz";
  foo.states["bar"].onDone.push_back(transition<Ctx>("barFinal"));
  foo.states["bar"].states["baz"].type = StateType::Final;
  foo.states["bar"].states["baz"].entry.push_back(mark("bazAction"));
  foo.states["barFinal"].type = StateType::Final;
  foo.states["barFinal"].entry.push_back(mark("barAction"));
  Rig r(createMachine<Ctx>(c));
  CHECK(r.state()->context.log ==
        std::vector<std::string>{"bazAction", "barAction", "fooAction"});
}

// final.test.ts: "should call output expressions on nested final nodes"
TEST_CASE("upstream/final: final output flows into the onDone event") {
  MachineConfig<Ctx> c;
  c.initial = "secret";
  auto& secret = c.states["secret"];
  secret.initial = "wait";
  secret.states["wait"].on["REQUEST_SECRET"] = "reveal";
  secret.states["reveal"].type = StateType::Final;
  secret.states["reveal"].output =
      std::any{std::function<std::any(const Ctx&, const Event&)>(
          [](const Ctx&, const Event&) { return std::any{std::string("the secret")}; })};
  secret.onDone.push_back(transition<Ctx>("success").act(
      assign<Ctx>([](Ctx& x, const Event& e) { x.revealedSecret = *e.dataAs<std::string>(); })));
  c.states["success"].type = StateType::Final;
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"REQUEST_SECRET"});
  CHECK(s->status == SnapshotStatus::Done);
  CHECK(s->context.revealedSecret == "the secret");
}

// final.test.ts: "state output should be able to use context updated by the entry
// action of the reached final state"
TEST_CASE("upstream/final: output sees context from the final state's entry assign") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  auto& a = c.states["a"];
  a.initial = "a1";
  a.states["a1"].on["NEXT"] = "a2";
  a.states["a2"].type = StateType::Final;
  a.states["a2"].entry.push_back(assign<Ctx>([](Ctx& x, const Event&) { x.count = 1; }));
  a.states["a2"].output = std::any{std::function<std::any(const Ctx&, const Event&)>(
      [](const Ctx& x, const Event&) { return std::any{x.count}; })};
  int received = -1;
  a.onDone.push_back(transition<Ctx>().act(assign<Ctx>(
      [&received](Ctx&, const Event& e) { received = *e.dataAs<int>(); })));
  Rig r(createMachine<Ctx>(c));
  r.send({"NEXT"});
  CHECK(received == 1);
}

// final.test.ts: "should call exit actions of parallel states in reversed document order
// when the machine reaches its final state after multiple regions transition"
TEST_CASE("upstream/final: machine done runs all exits in reverse document order") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  c.exit.push_back(mark("exit: __root__"));
  auto& a = c.states["a"];
  a.initial = "child_a1";
  a.exit.push_back(mark("exit: a"));
  a.states["child_a1"].exit.push_back(mark("exit: a.child_a1"));
  a.states["child_a1"].on["EV"] = "child_a2";
  a.states["child_a2"].type = StateType::Final;
  a.states["child_a2"].entry.push_back(mark("enter: a.child_a2"));
  a.states["child_a2"].exit.push_back(mark("exit: a.child_a2"));
  auto& b = c.states["b"];
  b.initial = "child_b1";
  b.exit.push_back(mark("exit: b"));
  b.states["child_b1"].exit.push_back(mark("exit: b.child_b1"));
  b.states["child_b1"].on["EV"] = "child_b2";
  b.states["child_b2"].type = StateType::Final;
  b.states["child_b2"].entry.push_back(mark("enter: b.child_b2"));
  b.states["child_b2"].exit.push_back(mark("exit: b.child_b2"));
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"EV"});
  CHECK(s->status == SnapshotStatus::Done);
  CHECK(s->context.log ==
        std::vector<std::string>{"exit: b.child_b1", "exit: a.child_a1", "enter: a.child_a2",
                                 "enter: b.child_b2", "exit: b.child_b2", "exit: b",
                                 "exit: a.child_a2", "exit: a", "exit: __root__"});
}

// final.test.ts: "should not complete a parallel root immediately when only some of
// its regions are in their final states" + "should not resolve output of a final
// state if its parent is a parallel state"
TEST_CASE("upstream/final: partial parallel finals do not complete; output not resolved") {
  MachineConfig<Ctx> c;
  c.type = StateType::Parallel;
  bool outputCalled = false;
  c.states["A"].type = StateType::Final;
  c.states["A"].output = std::any{std::function<std::any(const Ctx&, const Event&)>(
      [&outputCalled](const Ctx&, const Event&) {
        outputCalled = true;
        return std::any{};
      })};
  c.states["B"].initial = "B1";
  c.states["B"].states["B1"];
  c.states["B"].states["B2"].type = StateType::Final;
  Rig r(createMachine<Ctx>(c));
  CHECK(r.state()->status == SnapshotStatus::Active);
  CHECK_FALSE(outputCalled);
}

// final.test.ts: "should reach a final state when a parallel state reaches its final
// state and transitions to a top-level final state in response" + "onDone ... only once"
TEST_CASE("upstream/final: nested parallel completion cascades at start, onDone once") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  auto& a = c.states["a"];
  a.type = StateType::Parallel;
  a.onDone.push_back(transition<Ctx>("b").act(mark("onDoneAction")));
  auto& a1 = a.states["a1"];
  a1.type = StateType::Parallel;
  a1.states["a1a"].type = StateType::Final;
  a1.states["a1b"].type = StateType::Final;
  a.states["a2"].initial = "a2a";
  a.states["a2"].states["a2a"].type = StateType::Final;
  c.states["b"].type = StateType::Final;
  Rig r(createMachine<Ctx>(c));
  CHECK(r.state()->status == SnapshotStatus::Done);
  CHECK(r.state()->context.log == std::vector<std::string>{"onDoneAction"});
}

// deterministic.test.ts: "undefined transitions should forbid events" /
// "should bubble up events that nested states cannot handle"
TEST_CASE("upstream/determinism: forbidden events block bubbling") {
  MachineConfig<Ctx> c;
  c.initial = "green";
  c.states["green"].on["TIMER"] = "yellow";
  c.states["yellow"].on["TIMER"] = "red";
  auto& red = c.states["red"];
  red.initial = "walk";
  red.on["TIMER"] = "green";
  red.states["walk"].on["PED_COUNTDOWN"] = "wait";
  red.states["walk"].on["TIMER"];  // forbidden: empty transition list
  red.states["wait"].on["PED_COUNTDOWN"] = "stop";
  red.states["stop"];

  SUBCASE("forbidden entry shadows the ancestor handler") {
    Rig r(createMachine<Ctx>(c));
    r.send({"TIMER"});
    r.send({"TIMER"});  // -> red.walk
    auto s = r.send({"TIMER"});
    CHECK(s->matches("red.walk"));  // stays: walk forbids TIMER
  }
  SUBCASE("without a forbidden entry the event bubbles to the ancestor") {
    Rig r(createMachine<Ctx>(c));
    r.send({"TIMER"});
    r.send({"TIMER"});
    r.send({"PED_COUNTDOWN"});
    r.send({"PED_COUNTDOWN"});  // -> red.stop (no forbidden entry)
    auto s = r.send({"TIMER"});
    CHECK(s->matches("green"));
  }
}
