// Ported from xstate v5's own test suite: history.test.ts, after.test.ts,
// invoke.test.ts. Each case cites its upstream test title.
#include "doctest.h"
#include <xstate/actor.hpp>
#include <xstate/system.hpp>
#include <xstate/machine/machine.hpp>
#include <xstate/adapters/manual/manual_executor.hpp>
#include <xstate/adapters/manual/test_clock.hpp>
using namespace xstate;
using ms = std::chrono::milliseconds;

namespace {
struct Ctx {
  int n = 0;
};

using Snap = std::shared_ptr<const MachineSnapshot<Ctx>>;

// pure-machine rig for history cases
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

// actor rig for after/invoke cases
struct ActorRig {
  manual::ManualExecutor ex;
  manual::TestClock clk;
  std::shared_ptr<ActorSystem> sys;
  template <typename C>
  explicit ActorRig(std::shared_ptr<Machine<C>> m) {
    SystemOptions o;
    o.executor = &ex;
    o.clock = &clk;
    sys = createActorSystem<C>(m, o);
    sys->root()->start();
    ex.pump();
  }
  void advance(long long millis) {
    clk.advance(ms(millis));
    ex.pump();
  }
};

// parallel history playground shared by several upstream cases
MachineConfig<Ctx> parallelHistoryMachine() {
  MachineConfig<Ctx> c;
  c.initial = "off";
  c.states["off"].on["SWITCH"] = "on";
  c.states["off"].on["POWER"] = "on.hist";
  c.states["off"].on["DEEP_POWER"] = "on.deepHistory";
  c.states["off"].on["PARALLEL_HISTORY"] =
      transition<Ctx>("on.A.hist").target("on.K.hist");
  auto& on = c.states["on"];
  on.type = StateType::Parallel;
  on.on["POWER"] = "off";
  auto& A = on.states["A"];
  A.initial = "B";
  A.states["B"].on["INNER_A"] = "C";
  auto& C = A.states["C"];
  C.initial = "D";
  C.states["D"].on["INNER_A"] = "E";
  C.states["E"];
  A.states["hist"].type = StateType::History;
  A.states["deepHistory"].type = StateType::History;
  A.states["deepHistory"].history = HistoryType::Deep;
  auto& K = on.states["K"];
  K.initial = "L";
  K.states["L"].on["INNER_K"] = "M";
  auto& M = K.states["M"];
  M.initial = "N";
  M.states["N"].on["INNER_K"] = "O";
  M.states["O"];
  K.states["hist"].type = StateType::History;
  K.states["deepHistory"].type = StateType::History;
  K.states["deepHistory"].history = HistoryType::Deep;
  on.states["hist"].type = StateType::History;
  on.states["shallowHistory"].type = StateType::History;
  on.states["deepHistory"].type = StateType::History;
  on.states["deepHistory"].history = HistoryType::Deep;
  return c;
}
}  // namespace

// history.test.ts: "should go to the most recently visited state"
TEST_CASE("upstream/history: restores most recently visited child") {
  MachineConfig<Ctx> c;
  c.initial = "on";
  auto& on = c.states["on"];
  on.initial = "first";
  on.on["POWER"] = "off";
  on.states["first"].on["SWITCH"] = "second";
  on.states["second"];
  on.states["hist"].type = StateType::History;
  c.states["off"].on["POWER"] = "on.hist";
  Rig r(createMachine<Ctx>(c));
  r.send({"SWITCH"});
  r.send({"POWER"});
  CHECK(r.send({"POWER"})->matches("on.second"));
}

// history.test.ts: "should go to the initial state when no history present"
TEST_CASE("upstream/history: unrecorded history falls back to initial") {
  MachineConfig<Ctx> c;
  c.initial = "off";
  c.states["off"].on["POWER"] = "on.hist";
  auto& on = c.states["on"];
  on.initial = "first";
  on.states["first"];
  on.states["second"];
  on.states["hist"].type = StateType::History;
  Rig r(createMachine<Ctx>(c));
  CHECK(r.send({"POWER"})->matches("on.first"));
}

// history.test.ts: "should go to the configured default target when a history state
// is the initial state of the machine"
TEST_CASE("upstream/history: history as machine initial uses its default target") {
  MachineConfig<Ctx> c;
  c.initial = "foo";
  c.states["foo"].type = StateType::History;
  c.states["foo"].target = "bar";
  c.states["bar"];
  Rig r(createMachine<Ctx>(c));
  CHECK(r.state()->matches("bar"));
}

// history.test.ts: "...history state is the initial state of the transition's target"
TEST_CASE("upstream/history: history as initial of a transition target") {
  MachineConfig<Ctx> c;
  c.initial = "foo";
  c.states["foo"].on["NEXT"] = "bar";
  auto& bar = c.states["bar"];
  bar.initial = "baz";
  bar.states["baz"].type = StateType::History;
  bar.states["baz"].target = "qwe";
  bar.states["qwe"];
  Rig r(createMachine<Ctx>(c));
  CHECK(r.send({"NEXT"})->matches("bar.qwe"));
}

// history.test.ts: "deep history states > should go to the shallow history" /
// "should go to the deepest history"
TEST_CASE("upstream/history: shallow vs deep restoration depth") {
  MachineConfig<Ctx> c;
  c.initial = "on";
  c.states["off"].on["POWER"] = "on.history";
  auto& on = c.states["on"];
  on.initial = "first";
  on.on["POWER"] = "off";
  on.states["first"].on["SWITCH"] = "second";
  auto& second = on.states["second"];
  second.initial = "A";
  second.states["A"].on["INNER"] = "B";
  auto& B = second.states["B"];
  B.initial = "P";
  B.states["P"].on["INNER"] = "Q";
  B.states["Q"];
  on.states["history"].type = StateType::History;

  SUBCASE("shallow restores only the top-level child") {
    Rig r(createMachine<Ctx>(c));
    r.send({"SWITCH"});
    r.send({"INNER"});  // second.B.P
    r.send({"POWER"});
    CHECK(r.send({"POWER"})->matches("on.second.A"));  // subtree re-runs initial
  }
  SUBCASE("deep restores the exact leaf") {
    auto cc = c;
    cc.states["on"].states["history"].history = HistoryType::Deep;
    Rig r(createMachine<Ctx>(cc));
    r.send({"SWITCH"});
    r.send({"INNER"});   // second.B.P
    r.send({"INNER"});   // second.B.Q (P -> Q)
    r.send({"POWER"});
    CHECK(r.send({"POWER"})->matches("on.second.B.Q"));
  }
}

// history.test.ts: "parallel history states > should ignore parallel state history" /
// "should remember first level state history" / "should re-enter each regions of
// parallel state correctly" / "should re-enter multiple history states"
TEST_CASE("upstream/history: history in parallel states") {
  SUBCASE("shallow history of a parallel re-enters regions at their initials") {
    Rig r(createMachine<Ctx>(parallelHistoryMachine()));
    r.send({"SWITCH"});
    r.send({"INNER_A"});  // A: C.D
    r.send({"POWER"});
    auto s = r.send({"POWER"});  // on.hist (shallow)
    CHECK(s->matches("on.A.B"));
    CHECK(s->matches("on.K.L"));
  }
  SUBCASE("deep history of a parallel restores nested regions") {
    Rig r(createMachine<Ctx>(parallelHistoryMachine()));
    r.send({"SWITCH"});
    r.send({"INNER_A"});  // A: C.D
    r.send({"POWER"});
    auto s = r.send({"DEEP_POWER"});
    CHECK(s->matches("on.A.C.D"));
    CHECK(s->matches("on.K.L"));
  }
  SUBCASE("deep history restores every region's deepest configuration") {
    Rig r(createMachine<Ctx>(parallelHistoryMachine()));
    r.send({"SWITCH"});
    r.send({"INNER_A"});
    r.send({"INNER_A"});  // A: C.E
    r.send({"INNER_K"});
    r.send({"INNER_K"});  // K: M.O
    r.send({"POWER"});
    auto s = r.send({"DEEP_POWER"});
    CHECK(s->matches("on.A.C.E"));
    CHECK(s->matches("on.K.M.O"));
  }
  SUBCASE("multiple region-level shallow history targets in one transition") {
    Rig r(createMachine<Ctx>(parallelHistoryMachine()));
    r.send({"SWITCH"});
    r.send({"INNER_A"});
    r.send({"INNER_A"});
    r.send({"INNER_K"});
    r.send({"INNER_K"});
    r.send({"POWER"});
    auto s = r.send({"PARALLEL_HISTORY"});
    CHECK(s->matches("on.A.C.D"));  // shallow: C restored, D via initial
    CHECK(s->matches("on.K.M.N"));  // shallow: M restored, N via initial
  }
}

// history.test.ts: "should enter the parallel default configuration when a deep
// history state without a default target is targeted and its parent parallel
// state was never visited yet"
TEST_CASE("upstream/history: unvisited parallel deep history enters defaults") {
  MachineConfig<Ctx> c;
  c.initial = "off";
  c.states["off"].on["GO"] = "on.hist";
  auto& on = c.states["on"];
  on.type = StateType::Parallel;
  on.states["regA"].initial = "a1";
  on.states["regA"].states["a1"];
  on.states["regA"].states["a2"];
  on.states["regB"].initial = "b1";
  on.states["regB"].states["b1"];
  on.states["regB"].states["b2"];
  on.states["hist"].type = StateType::History;
  on.states["hist"].history = HistoryType::Deep;
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"GO"});
  CHECK(s->matches("on.regA.a1"));
  CHECK(s->matches("on.regB.b1"));
}

// history.test.ts: "internal transition to a history state should enter default
// history state configuration if the containing state has never been exited yet"
TEST_CASE("upstream/history: internal history target before any exit uses initial") {
  MachineConfig<Ctx> c;
  c.initial = "first";
  c.states["first"].on["NEXT"] = "second.other";
  auto& second = c.states["second"];
  second.initial = "nested";
  second.on["NEXT"] = ".hist";
  second.states["nested"];
  second.states["other"];
  second.states["hist"].type = StateType::History;
  Rig r(createMachine<Ctx>(c));
  CHECK(r.send({"NEXT"})->matches("second.other"));
  CHECK(r.send({"NEXT"})->matches("second.nested"));  // no history recorded yet
}

// history.test.ts: "multistage history states > should go to the most recently visited state"
TEST_CASE("upstream/history: history survives intermediate states") {
  MachineConfig<Ctx> c;
  c.initial = "running";
  auto& running = c.states["running"];
  running.initial = "normal";
  running.on["POWER"] = "off";
  running.states["normal"].on["SWITCH_TURBO"] = "turbo";
  running.states["turbo"].on["SWITCH_TURBO"] = "normal";
  running.states["H"].type = StateType::History;
  c.states["starting"].on["STARTED"] = "running.H";
  c.states["off"].on["POWER"] = "starting";
  Rig r(createMachine<Ctx>(c));
  r.send({"SWITCH_TURBO"});
  r.send({"POWER"});
  r.send({"POWER"});
  CHECK(r.send({"STARTED"})->matches("running.turbo"));
}

// after.test.ts: "should transition after delay"
TEST_CASE("upstream/after: numeric delay boundary timing") {
  MachineConfig<Ctx> c;
  c.initial = "green";
  c.states["green"].after[1000] = "yellow";
  c.states["yellow"].after[1000] = "red";
  c.states["red"].after[1000] = "green";
  ActorRig r(createMachine<Ctx>(c));
  CHECK(machineSnapshot<Ctx>(r.sys->root()->getSnapshot())->matches("green"));
  r.advance(500);
  CHECK(machineSnapshot<Ctx>(r.sys->root()->getSnapshot())->matches("green"));
  r.advance(510);
  CHECK(machineSnapshot<Ctx>(r.sys->root()->getSnapshot())->matches("yellow"));
}

// after.test.ts: "should not try to clear an undefined timeout when exiting source
// state of a delayed transition" (issue #5001)
namespace {
class CountingClock : public Clock {
 public:
  TimerId setTimeout(Task task, ms delay) override { return inner.setTimeout(std::move(task), delay); }
  void clearTimeout(TimerId id) override {
    clears++;
    inner.clearTimeout(id);
  }
  manual::TestClock inner;
  int clears = 0;
};
}  // namespace

TEST_CASE("upstream/after: consumed timer is not cleared on state exit") {
  MachineConfig<Ctx> c;
  c.initial = "green";
  c.states["green"].after[1] = "yellow";
  c.states["yellow"];
  manual::ManualExecutor ex;
  CountingClock clk;
  SystemOptions o;
  o.executor = &ex;
  o.clock = &clk;
  auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c), o);
  sys->root()->start();
  ex.pump();
  clk.inner.advance(ms(5));
  ex.pump();
  CHECK(machineSnapshot<Ctx>(sys->root()->getSnapshot())->matches("yellow"));
  CHECK(clk.clears == 0);  // the fired timer must not be "cancelled" after the fact
}

// after.test.ts: "delay expressions > should evaluate the expression (function)
// to determine the delay"
TEST_CASE("upstream/after: named delay resolved from context once per entry") {
  struct DCtx {
    long long delay = 500;
  };
  MachineConfig<DCtx> c;
  c.initial = "inactive";
  c.states["inactive"].after["myDelay"] = "active";
  c.states["active"];
  MachineOptions<DCtx> mo;
  int calls = 0;
  mo.delays["myDelay"] = std::function<long long(const DCtx&, const Event&)>(
      [&calls](const DCtx& x, const Event&) {
        calls++;
        return x.delay;
      });
  manual::ManualExecutor ex;
  manual::TestClock clk;
  SystemOptions o;
  o.executor = &ex;
  o.clock = &clk;
  auto sys = createActorSystem<DCtx>(createMachine<DCtx>(c, mo), o);
  sys->root()->start();
  ex.pump();
  CHECK(calls == 1);
  clk.advance(ms(300));
  ex.pump();
  CHECK(machineSnapshot<DCtx>(sys->root()->getSnapshot())->matches("inactive"));
  clk.advance(ms(200));
  ex.pump();
  CHECK(machineSnapshot<DCtx>(sys->root()->getSnapshot())->matches("active"));
}

// invoke.test.ts: "parent to child > should transition correctly if child invocation
// causes it to directly go to final state"
TEST_CASE("upstream/invoke: entry sendTo drives child to final; parent lands on onDone target") {
  struct PCtx {};
  struct CCtx {};
  MachineConfig<CCtx> child;
  child.id = "child";
  child.initial = "one";
  child.states["one"].on["NEXT"] = "two";
  child.states["two"].type = StateType::Final;
  auto childMachine = createMachine<CCtx>(child);

  MachineConfig<PCtx> parent;
  parent.id = "parent";
  parent.initial = "one";
  parent.states["one"].invoke.push_back(invoke<PCtx>("child").id("foo-child").onDone("two"));
  parent.states["one"].entry.push_back(sendTo<PCtx>("foo-child", Event{"NEXT"}));
  parent.states["two"].on["NEXT"] = "three";
  parent.states["three"].type = StateType::Final;
  MachineOptions<PCtx> o;
  o.actors["child"] = [childMachine](const std::any&) -> std::shared_ptr<ActorLogic> {
    return childMachine;
  };
  ActorRig r(createMachine<PCtx>(parent, o));
  auto s = machineSnapshot<PCtx>(r.sys->root()->getSnapshot());
  REQUIRE(s != nullptr);
  CHECK(s->matches("two"));  // NOT "three": the child's NEXT must not leak to the parent
}

// invoke.test.ts: "should start services (explicit machine, invoke = config)"
TEST_CASE("upstream/invoke: child output reaches the parent's onDone guard") {
  struct PCtx {};
  struct CCtx {};
  MachineConfig<CCtx> child;
  child.initial = "pending";
  child.states["pending"].on["RESOLVE"] = "success";
  child.states["pending"].entry.push_back(raise<CCtx>(Event{"RESOLVE"}));
  child.states["success"].type = StateType::Final;
  child.states["success"].output = std::any{std::string("David")};
  auto childMachine = createMachine<CCtx>(child);

  MachineConfig<PCtx> parent;
  parent.initial = "idle";
  parent.states["idle"].on["GO_TO_WAITING"] = "waiting";
  parent.states["waiting"].invoke.push_back(
      invoke<PCtx>("fetch").onDone(
          transition<PCtx>("received").guarded(guardFn<PCtx>([](const PCtx&, const Event& e) {
            const auto* name = e.dataAs<std::string>();
            return name != nullptr && *name == "David";
          }))));
  parent.states["received"].type = StateType::Final;
  MachineOptions<PCtx> o;
  o.actors["fetch"] = [childMachine](const std::any&) -> std::shared_ptr<ActorLogic> {
    return childMachine;
  };
  ActorRig r(createMachine<PCtx>(parent, o));
  r.sys->root()->send({"GO_TO_WAITING"});
  r.ex.pump();
  CHECK(r.sys->root()->getSnapshot()->status == SnapshotStatus::Done);
}

// invoke.test.ts: "parent to child > should work with invocations defined in
// orthogonal state nodes"
TEST_CASE("upstream/invoke: instantly-final child completes a parallel parent") {
  struct PCtx {};
  struct CCtx {};
  MachineConfig<CCtx> pong;
  pong.id = "pong";
  pong.initial = "active";
  pong.states["active"].type = StateType::Final;
  pong.states["active"].output = std::any{std::string("pingpong")};
  auto pongMachine = createMachine<CCtx>(pong);

  MachineConfig<PCtx> ping;
  ping.id = "ping";
  ping.type = StateType::Parallel;
  auto& one = ping.states["one"];
  one.initial = "active";
  one.states["active"].invoke.push_back(
      invoke<PCtx>("pong").onDone(
          transition<PCtx>("success").guarded(guardFn<PCtx>([](const PCtx&, const Event& e) {
            const auto* secret = e.dataAs<std::string>();
            return secret != nullptr && *secret == "pingpong";
          }))));
  one.states["success"].type = StateType::Final;
  MachineOptions<PCtx> o;
  o.actors["pong"] = [pongMachine](const std::any&) -> std::shared_ptr<ActorLogic> {
    return pongMachine;
  };
  ActorRig r(createMachine<PCtx>(ping, o));
  CHECK(r.sys->root()->getSnapshot()->status == SnapshotStatus::Done);
}
