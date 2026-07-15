#include "doctest.h"
#include <xstate/actor.hpp>
#include <xstate/system.hpp>
#include <xstate/machine/machine.hpp>
#include <xstate/adapters/manual/manual_executor.hpp>
#include <xstate/adapters/manual/test_clock.hpp>
using namespace xstate;
using ms = std::chrono::milliseconds;

namespace {
struct CCtx { int n = 0; };
struct PCtx { std::string result; };

std::shared_ptr<Machine<CCtx>> childMachine() {
  MachineConfig<CCtx> c;
  c.initial = "wait";
  c.states["wait"].on["PING"] = "fin";
  c.states["fin"].type = StateType::Final;
  c.states["fin"].output = std::any{std::string("pong")};
  return createMachine<CCtx>(c);
}

std::shared_ptr<Machine<PCtx>> parentMachine() {
  MachineConfig<PCtx> c;
  c.initial = "running";
  c.states["running"].invoke.push_back(
      invoke<PCtx>("child").id("kid")
          .onDone(transition<PCtx>("ok").act(assign<PCtx>([](PCtx& x, const Event& e) {
            x.result = *e.dataAs<std::string>();
          }))));
  c.states["running"].on["FORWARD"] =
      transition<PCtx>().act(sendTo<PCtx>("kid", Event{"PING"}));
  c.states["ok"];
  MachineOptions<PCtx> o;
  o.actors["child"] = [](const std::any&) -> std::shared_ptr<ActorLogic> {
    return childMachine();
  };
  return createMachine<PCtx>(c, o);
}

template <typename C>
struct Rig {
  manual::ManualExecutor ex;
  manual::TestClock clk;
  std::shared_ptr<ActorSystem> sys;
  explicit Rig(std::shared_ptr<Machine<C>> m, ActorOptions a = {}) {
    SystemOptions o;
    o.executor = &ex;
    o.clock = &clk;
    sys = createActorSystem<C>(m, o, std::move(a));
    sys->root()->start();
    ex.pump();
  }
};
}  // namespace

TEST_CASE("structured persist/restore round-trips a running actor tree") {
  auto m = parentMachine();
  Rig<PCtx> r1(m);
  REQUIRE(machineSnapshot<PCtx>(r1.sys->root()->getSnapshot())->children.count("kid") == 1);
  std::any persisted = r1.sys->root()->getPersistedSnapshot();

  ActorOptions restore;
  restore.snapshot = persisted;
  Rig<PCtx> r2(m, restore);
  auto snap = machineSnapshot<PCtx>(r2.sys->root()->getSnapshot());
  REQUIRE(snap != nullptr);
  CHECK(snap->matches("running"));
  REQUIRE(snap->children.count("kid") == 1);
  r2.sys->root()->send({"FORWARD"});  // restored child must respond
  r2.ex.pump();
  auto done = machineSnapshot<PCtx>(r2.sys->root()->getSnapshot());
  CHECK(done->matches("ok"));
  CHECK(done->context.result == "pong");
}

TEST_CASE("deep history survives persist/restore") {
  struct HCtx {};
  MachineConfig<HCtx> c;
  c.initial = "on";
  c.states["on"].initial = "video";
  c.states["on"].states["hist"].type = StateType::History;
  c.states["on"].states["hist"].history = HistoryType::Deep;
  c.states["on"].states["video"].initial = "playing";
  c.states["on"].states["video"].states["playing"].on["PAUSE"] = "paused";
  c.states["on"].states["video"].states["paused"];
  c.states["on"].on["POWER"] = "off";
  c.states["off"].on["POWER"] = "on.hist";
  auto m = createMachine<HCtx>(c);
  Rig<HCtx> r1(m);
  r1.sys->root()->send({"PAUSE"});
  r1.sys->root()->send({"POWER"});  // off, history = video.paused
  r1.ex.pump();
  std::any persisted = r1.sys->root()->getPersistedSnapshot();

  ActorOptions restore;
  restore.snapshot = persisted;
  Rig<HCtx> r2(m, restore);
  r2.sys->root()->send({"POWER"});
  r2.ex.pump();
  CHECK(machineSnapshot<HCtx>(r2.sys->root()->getSnapshot())->matches("on.video.paused"));
}

TEST_CASE("after timers restart from the full delay on restore") {
  struct TCtx {};
  MachineConfig<TCtx> c;
  c.initial = "green";
  c.states["green"].after[1000] = "yellow";
  c.states["yellow"];
  auto m = createMachine<TCtx>(c);
  Rig<TCtx> r1(m);
  std::any persisted = r1.sys->root()->getPersistedSnapshot();

  ActorOptions restore;
  restore.snapshot = persisted;
  Rig<TCtx> r2(m, restore);
  r2.clk.advance(ms(999));
  r2.ex.pump();
  CHECK(machineSnapshot<TCtx>(r2.sys->root()->getSnapshot())->matches("green"));
  r2.clk.advance(ms(1));
  r2.ex.pump();
  CHECK(machineSnapshot<TCtx>(r2.sys->root()->getSnapshot())->matches("yellow"));
}

namespace {
struct JCtx {
  int n = 0;
  std::string name;
};
}  // namespace
XSTATE_CONTEXT_FIELDS(JCtx, n, name)

TEST_CASE("JSON persistence round-trips value and context via the codec") {
  MachineConfig<JCtx> c;
  c.initial = "a";
  c.states["a"].on["SET"] = transition<JCtx>("b").act(assign<JCtx>([](JCtx& x, const Event&) {
    x.n = 42;
    x.name = "phil \"quoted\"";
  }));
  c.states["b"];
  auto m = createMachine<JCtx>(c);
  m->setContextCodec(xstateCodecFor(static_cast<JCtx*>(nullptr)));
  Rig<JCtx> r1(m);
  r1.sys->root()->send({"SET"});
  r1.ex.pump();
  std::string json = r1.sys->root()->getPersistedSnapshotJson();
  CHECK(json.find("\"value\":\"b\"") != std::string::npos);

  ActorOptions restore;
  restore.snapshot = m->parseSnapshotJson(json);
  Rig<JCtx> r2(m, restore);
  auto snap = machineSnapshot<JCtx>(r2.sys->root()->getSnapshot());
  REQUIRE(snap != nullptr);
  CHECK(snap->matches("b"));
  CHECK(snap->context.n == 42);
  CHECK(snap->context.name == "phil \"quoted\"");
}

TEST_CASE("JSON persistence without a codec throws ConfigError") {
  struct NCtx { int n = 0; };
  MachineConfig<NCtx> c;
  c.initial = "a";
  c.states["a"];
  auto m = createMachine<NCtx>(c);
  Rig<NCtx> r(m);
  CHECK_THROWS_AS(r.sys->root()->getPersistedSnapshotJson(), ConfigError);
}
