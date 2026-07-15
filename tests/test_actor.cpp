#include "doctest.h"
#include <xstate/actor.hpp>
#include <xstate/system.hpp>
#include <xstate/machine/machine.hpp>
#include <xstate/adapters/manual/manual_executor.hpp>
#include <xstate/adapters/manual/test_clock.hpp>
using namespace xstate;

namespace {
struct Ctx { int n = 0; };

std::shared_ptr<Machine<Ctx>> counter() {
  MachineConfig<Ctx> c;
  c.initial = "idle";
  c.states["idle"].on["INC"] =
      transition<Ctx>().act(assign<Ctx>([](Ctx& x, const Event&) { x.n++; }));
  c.states["idle"].on["BOOM"] = transition<Ctx>().act(
      assign<Ctx>([](Ctx&, const Event&) { throw std::runtime_error("boom"); }));
  c.states["idle"].on["DONE"] = "fin";
  c.states["fin"].type = StateType::Final;
  return createMachine<Ctx>(c);
}

struct Rig {
  manual::ManualExecutor ex;
  manual::TestClock clk;
  std::shared_ptr<ActorSystem> sys;
  Rig() {
    SystemOptions o;
    o.executor = &ex;
    o.clock = &clk;
    sys = createActorSystem<Ctx>(counter(), o);
  }
};
}  // namespace

TEST_CASE("nothing runs before pump; send is queued through executor") {
  Rig r;
  r.sys->root()->start();
  CHECK(machineSnapshot<Ctx>(r.sys->root()->getSnapshot()) == nullptr);  // not started yet
  r.ex.pump();
  r.sys->root()->send({"INC"});
  r.sys->root()->send({"INC"});
  CHECK(machineSnapshot<Ctx>(r.sys->root()->getSnapshot())->context.n == 0);
  r.ex.pump();
  CHECK(machineSnapshot<Ctx>(r.sys->root()->getSnapshot())->context.n == 2);
}

TEST_CASE("subscribers notified once per changed snapshot, on pump") {
  Rig r;
  int notifications = 0;
  r.sys->root()->subscribe([&](SnapshotPtr) { notifications++; });
  r.sys->root()->start();
  r.ex.pump();
  int afterStart = notifications;
  CHECK(afterStart >= 1);
  r.sys->root()->send({"UNKNOWN"});
  r.ex.pump();  // unhandled -> no notification
  CHECK(notifications == afterStart);
  r.sys->root()->send({"INC"});
  r.ex.pump();
  CHECK(notifications == afterStart + 1);
}

TEST_CASE("thrown action becomes Error status, executor survives") {
  Rig r;
  r.sys->root()->start();
  r.ex.pump();
  r.sys->root()->send({"BOOM"});
  r.ex.pump();
  CHECK(r.sys->root()->getSnapshot()->status == SnapshotStatus::Error);
  r.sys->root()->send({"INC"});
  r.ex.pump();  // dropped silently
  CHECK(r.sys->root()->getSnapshot()->status == SnapshotStatus::Error);
}

TEST_CASE("stop drops subsequent sends; final state reports Done") {
  Rig r;
  r.sys->root()->start();
  r.ex.pump();
  r.sys->root()->send({"DONE"});
  r.ex.pump();
  CHECK(r.sys->root()->getSnapshot()->status == SnapshotStatus::Done);

  Rig r2;
  r2.sys->root()->start();
  r2.ex.pump();
  r2.sys->stop();
  r2.ex.pump();
  CHECK(r2.sys->root()->getSnapshot()->status == SnapshotStatus::Stopped);
}
