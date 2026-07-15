#include "doctest.h"
#include <xstate/machine/machine.hpp>
using namespace xstate;

namespace {
struct Ctx {};

MachineConfig<Ctx> player(HistoryType t) {
  MachineConfig<Ctx> c;
  c.initial = "on";
  c.states["on"].initial = "video";
  c.states["on"].states["hist"].type = StateType::History;
  c.states["on"].states["hist"].history = t;
  c.states["on"].states["video"].initial = "playing";
  c.states["on"].states["video"].states["playing"].on["PAUSE"] = "paused";
  c.states["on"].states["video"].states["paused"];
  c.states["on"].states["music"];
  c.states["on"].states["video"].on["MUSIC"] = "music";
  c.states["on"].on["POWER"] = "off";
  c.states["off"].on["POWER"] = "on.hist";
  return c;
}
}  // namespace

TEST_CASE("shallow history restores child but re-runs its initial") {
  auto m = createMachine<Ctx>(player(HistoryType::Shallow));
  ActorScope s;
  auto s1 = m->transition(m->getInitialSnapshot(s, {}), Event{"PAUSE"}, s);  // video.paused
  auto s2 = m->transition(s1, Event{"POWER"}, s);                            // off
  auto s3 = machineSnapshot<Ctx>(m->transition(s2, Event{"POWER"}, s));      // on.hist
  CHECK(s3->matches("on.video.playing"));  // shallow: video restored, initial re-entered
}

TEST_CASE("deep history restores the exact leaf") {
  auto m = createMachine<Ctx>(player(HistoryType::Deep));
  ActorScope s;
  auto s1 = m->transition(m->getInitialSnapshot(s, {}), Event{"PAUSE"}, s);
  auto s2 = m->transition(s1, Event{"POWER"}, s);
  auto s3 = machineSnapshot<Ctx>(m->transition(s2, Event{"POWER"}, s));
  CHECK(s3->matches("on.video.paused"));
}

TEST_CASE("unvisited history uses default target, else parent initial") {
  // start at "off" so `on` has never been visited when hist is targeted
  auto cfgD = player(HistoryType::Shallow);
  cfgD.initial = "off";
  cfgD.states["on"].states["hist"].target = "music";
  auto mD = createMachine<Ctx>(cfgD);
  ActorScope s;
  CHECK(machineSnapshot<Ctx>(
            mD->transition(mD->getInitialSnapshot(s, {}), Event{"POWER"}, s))
            ->matches("on.music"));

  auto cfgI = player(HistoryType::Shallow);
  cfgI.initial = "off";
  auto mI = createMachine<Ctx>(cfgI);
  CHECK(machineSnapshot<Ctx>(
            mI->transition(mI->getInitialSnapshot(s, {}), Event{"POWER"}, s))
            ->matches("on.video.playing"));
}

TEST_CASE("recorded history wins over default target") {
  auto cfg = player(HistoryType::Shallow);
  cfg.states["on"].states["hist"].target = "music";
  auto m = createMachine<Ctx>(cfg);
  ActorScope s;
  auto o1 = m->transition(m->getInitialSnapshot(s, {}), Event{"POWER"}, s);  // visited video
  CHECK(machineSnapshot<Ctx>(m->transition(o1, Event{"POWER"}, s))->matches("on.video.playing"));
}
