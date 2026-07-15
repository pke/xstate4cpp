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
  c.states["idle"].on["PING"] = transition<Ctx>().act(emit<Ctx>(Event{"notify", 7}));
  return createMachine<Ctx>(c);
}

struct Rig {
  manual::ManualExecutor ex;
  manual::TestClock clk;
  std::shared_ptr<ActorSystem> sys;
  std::vector<InspectionEvent> seen;
  Rig() {
    SystemOptions o;
    o.executor = &ex;
    o.clock = &clk;
    sys = createActorSystem<Ctx>(counter(), o);
    sys->inspect([this](const InspectionEvent& ev) { seen.push_back(ev); });
  }
};
}  // namespace

TEST_CASE("start yields ActorCreated, EventReceived(init), SnapshotChanged in order") {
  Rig r;
  r.sys->root()->start();
  r.ex.pump();
  REQUIRE(r.seen.size() >= 3);
  CHECK(r.seen[0].kind == InspectionEvent::Kind::ActorCreated);
  CHECK(r.seen[0].sessionId == "x:0");
  CHECK(r.seen[1].kind == InspectionEvent::Kind::EventReceived);
  CHECK(r.seen[1].event.type == "xstate.init");
  CHECK(r.seen[2].kind == InspectionEvent::Kind::SnapshotChanged);
}

TEST_CASE("send yields EventReceived always, SnapshotChanged only when changed") {
  Rig r;
  r.sys->root()->start();
  r.ex.pump();
  r.seen.clear();
  r.sys->root()->send({"UNKNOWN"});
  r.ex.pump();
  REQUIRE(r.seen.size() == 1);  // event only, no snapshot change
  CHECK(r.seen[0].kind == InspectionEvent::Kind::EventReceived);
  r.seen.clear();
  r.sys->root()->send({"INC"});
  r.ex.pump();
  REQUIRE(r.seen.size() == 2);
  CHECK(r.seen[0].kind == InspectionEvent::Kind::EventReceived);
  CHECK(r.seen[1].kind == InspectionEvent::Kind::SnapshotChanged);
}

TEST_CASE("invoked child appears with a distinct sessionId") {
  struct PCtx {};
  MachineConfig<PCtx> c;
  c.initial = "run";
  c.states["run"].invoke.push_back(invoke<PCtx>("kid"));
  MachineOptions<PCtx> o;
  o.actors["kid"] = [](const std::any&) -> std::shared_ptr<ActorLogic> {
    MachineConfig<Ctx> k;
    k.initial = "idle";
    k.states["idle"];
    return createMachine<Ctx>(k);
  };
  manual::ManualExecutor ex;
  manual::TestClock clk;
  SystemOptions so;
  so.executor = &ex;
  so.clock = &clk;
  auto sys = createActorSystem<PCtx>(createMachine<PCtx>(c, o), so);
  std::vector<std::string> createdSessions;
  sys->inspect([&](const InspectionEvent& ev) {
    if (ev.kind == InspectionEvent::Kind::ActorCreated)
      createdSessions.push_back(ev.sessionId);
  });
  sys->root()->start();
  ex.pump();
  REQUIRE(createdSessions.size() == 2);
  CHECK(createdSessions[0] != createdSessions[1]);
}

TEST_CASE("toJson uses the @xstate wire format") {
  Rig r;
  r.sys->root()->start();
  r.ex.pump();
  r.seen.clear();
  r.sys->root()->send({"INC"});
  r.ex.pump();
  REQUIRE(r.seen.size() == 2);
  CHECK(r.seen[0].toJson() ==
        R"({"type":"@xstate.event","sessionId":"x:0","event":{"type":"INC"}})");
  CHECK(r.seen[1].toJson() ==
        R"({"type":"@xstate.snapshot","sessionId":"x:0","snapshot":{"status":"active","value":"idle"}})");
}

TEST_CASE("emit reaches on() handlers with the event payload") {
  Rig r;
  int got = 0;
  r.sys->root()->on("notify", [&](const Event& e) { got = *e.dataAs<int>(); });
  r.sys->root()->start();
  r.ex.pump();
  r.sys->root()->send({"PING"});
  r.ex.pump();
  CHECK(got == 7);
}
