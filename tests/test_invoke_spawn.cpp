#include "doctest.h"
#include <xstate/actor.hpp>
#include <xstate/system.hpp>
#include <xstate/machine/machine.hpp>
#include <xstate/adapters/manual/manual_executor.hpp>
#include <xstate/adapters/manual/test_clock.hpp>
using namespace xstate;

namespace {
struct CCtx { int n = 0; };
struct PCtx {
  std::string result;
  std::string err;
};

// child machine: waits for PING, then finishes with output "pong"
std::shared_ptr<Machine<CCtx>> childMachine() {
  MachineConfig<CCtx> c;
  c.initial = "wait";
  c.states["wait"].on["PING"] = "fin";
  c.states["fin"].type = StateType::Final;
  c.states["fin"].output = std::any{std::string("pong")};
  return createMachine<CCtx>(c);
}

std::shared_ptr<Machine<PCtx>> parentMachine(bool childThrows) {
  MachineConfig<PCtx> c;
  c.initial = "running";
  c.states["running"].invoke.push_back(
      invoke<PCtx>("child").id("kid")
          .onDone(transition<PCtx>("ok").act(assign<PCtx>([](PCtx& x, const Event& e) {
            x.result = *e.dataAs<std::string>();
          })))
          .onError(transition<PCtx>("failed")));
  c.states["running"].on["FORWARD"] =
      transition<PCtx>().act(sendTo<PCtx>("kid", Event{"PING"}));
  c.states["ok"];
  c.states["failed"];
  MachineOptions<PCtx> o;
  o.actors["child"] = [childThrows](const std::any&) -> std::shared_ptr<ActorLogic> {
    if (childThrows) {
      MachineConfig<CCtx> bad;
      bad.initial = "a";
      bad.states["a"].entry.push_back(
          assign<CCtx>([](CCtx&, const Event&) { throw std::runtime_error("child boom"); }));
      return createMachine<CCtx>(bad);
    }
    return childMachine();
  };
  return createMachine<PCtx>(c, o);
}

struct Rig {
  manual::ManualExecutor ex;
  manual::TestClock clk;
  std::shared_ptr<ActorSystem> sys;
  explicit Rig(std::shared_ptr<Machine<PCtx>> m) {
    SystemOptions o;
    o.executor = &ex;
    o.clock = &clk;
    sys = createActorSystem<PCtx>(m, o);
    sys->root()->start();
    ex.pump();
  }
};
}  // namespace

TEST_CASE("invoke spawns child, sendTo child by invoke id, onDone receives output") {
  Rig r(parentMachine(false));
  auto snap = machineSnapshot<PCtx>(r.sys->root()->getSnapshot());
  REQUIRE(snap->children.count("kid") == 1);
  r.sys->root()->send({"FORWARD"});
  r.ex.pump();
  auto done = machineSnapshot<PCtx>(r.sys->root()->getSnapshot());
  CHECK(done->matches("ok"));
  CHECK(done->context.result == "pong");
  CHECK(done->children.count("kid") == 0);  // stopped & removed
}

TEST_CASE("child error routes to onError") {
  Rig r(parentMachine(true));
  CHECK(machineSnapshot<PCtx>(r.sys->root()->getSnapshot())->matches("failed"));
}

TEST_CASE("invoked child is force-stopped when the invoking state exits") {
  MachineConfig<PCtx> c;
  c.initial = "running";
  c.states["running"].invoke.push_back(
      invoke<PCtx>("child").id("kid").onDone(transition<PCtx>("ok")).onError(
          transition<PCtx>("failed")));
  c.states["running"].on["ABORT"] = "aborted";
  c.states["ok"];
  c.states["failed"];
  c.states["aborted"];
  MachineOptions<PCtx> o;
  o.actors["child"] = [](const std::any&) -> std::shared_ptr<ActorLogic> {
    return childMachine();
  };
  Rig r(createMachine<PCtx>(c, o));
  auto childRef = machineSnapshot<PCtx>(r.sys->root()->getSnapshot())->children.at("kid");
  r.sys->root()->send({"ABORT"});
  r.ex.pump();
  CHECK(childRef->getSnapshot()->status == SnapshotStatus::Stopped);  // auto-stopped, not Done
  CHECK(machineSnapshot<PCtx>(r.sys->root()->getSnapshot())->children.count("kid") == 0);
}

TEST_CASE("receptionist holds only explicitly registered systemIds") {
  Rig r(parentMachine(false));
  CHECK(r.sys->get("kid") == nullptr);   // invoke ids are actor-local, never system-wide
  CHECK(r.sys->get("root") == nullptr);  // no implicit root registration

  manual::ManualExecutor ex2;
  manual::TestClock clk2;
  SystemOptions o2;
  o2.executor = &ex2;
  o2.clock = &clk2;
  ActorOptions a2;
  a2.systemId = "mainframe";
  auto sys2 = createActorSystem<PCtx>(parentMachine(false), o2, a2);
  CHECK(sys2->get("mainframe") == sys2->root());
}
