// Minimal xstate4cpp example: a toggle machine on a native event loop.
#include <chrono>
#include <cstdio>
#include <thread>

#include <xstate/xstate.hpp>
#include "example_loop.hpp"

struct Ctx {
  int toggles = 0;
};

int main() {
  using namespace xstate;

  MachineConfig<Ctx> c;
  c.id = "toggle";
  c.initial = "inactive";
  c.states["inactive"].on["TOGGLE"] =
      transition<Ctx>("active").act(assign<Ctx>([](Ctx& x, const Event&) { x.toggles++; }));
  c.states["active"].on["TOGGLE"] = "inactive";

  exloop::EventLoop loop;
  SystemOptions o;
  o.executor = &loop;
  o.clock = &loop;
  auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c), o);

  sys->root()->subscribe([](SnapshotPtr snap) {
    auto s = machineSnapshot<Ctx>(snap);
    if (s) std::printf("state: %s (toggles: %d)\n", s->value.toString().c_str(), s->context.toggles);
  });

  sys->root()->start();
  for (int i = 0; i < 4; ++i) sys->root()->send({"TOGGLE"});
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sys->stop();
  return 0;
}
