// Delayed transitions (`after`) driving a traffic light on real timers.
#include <chrono>
#include <cstdio>
#include <thread>

#include <xstate/xstate.hpp>
#include "example_loop.hpp"

struct Ctx {
  int cycles = 0;
};

int main() {
  using namespace xstate;

  MachineConfig<Ctx> c;
  c.id = "light";
  c.initial = "green";
  c.states["green"].after[300] = "yellow";
  c.states["yellow"].after[100] = "red";
  c.states["red"].after[200] = transition<Ctx>("green").act(
      assign<Ctx>([](Ctx& x, const Event&) { x.cycles++; }));

  exloop::EventLoop loop;
  SystemOptions o;
  o.executor = &loop;
  o.clock = &loop;
  auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c), o);

  sys->root()->subscribe([](SnapshotPtr snap) {
    auto s = machineSnapshot<Ctx>(snap);
    if (s) std::printf("light: %s\n", s->value.toString().c_str());
  });

  sys->root()->start();
  // run two full cycles, then shut down cleanly
  while (true) {
    auto s = machineSnapshot<Ctx>(sys->root()->getSnapshot());
    if (s && s->context.cycles >= 2) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  sys->stop();
  std::printf("done after 2 cycles\n");
  return 0;
}
