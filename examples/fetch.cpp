// fromAsync + invoke with onDone/onError and a retry guard.
#include <chrono>
#include <cstdio>
#include <thread>

#include <xstate/xstate.hpp>
#include "example_loop.hpp"

struct Ctx {
  int attempts = 0;
  std::string user;
};

int main() {
  using namespace xstate;

  MachineConfig<Ctx> c;
  c.id = "fetch";
  c.initial = "loading";
  auto& loading = c.states["loading"];
  loading.entry.push_back(assign<Ctx>([](Ctx& x, const Event&) { x.attempts++; }));
  loading.invoke.push_back(
      invoke<Ctx>("fetchUser")
          .onDone(transition<Ctx>("success").act(assign<Ctx>([](Ctx& x, const Event& e) {
            x.user = *e.dataAs<std::string>();
          })))
          .onError(transition<Ctx>("loading")
                       .guarded("canRetry")
                       .act(log<Ctx>("retrying...")))
          .onError(transition<Ctx>("failure")));
  c.states["success"].type = StateType::Final;
  c.states["failure"].type = StateType::Final;

  MachineOptions<Ctx> o;
  o.guards["canRetry"] = [](const Ctx& x, const Event&) { return x.attempts < 3; };
  o.actors["fetchUser"] = [](const std::any&) -> std::shared_ptr<ActorLogic> {
    return fromAsync([](Resolver r, const std::any&) {
      // simulate async I/O completing on a foreign thread
      std::thread([r] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        static int calls = 0;
        if (++calls < 3) r.reject(std::string("network down"));
        else r.resolve(std::string("Grace Hopper"));
      }).detach();
    });
  };

  exloop::EventLoop loop;
  SystemOptions so;
  so.executor = &loop;
  so.clock = &loop;
  auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c, o), so);
  sys->root()->start();

  for (int i = 0; i < 100; ++i) {
    auto s = machineSnapshot<Ctx>(sys->root()->getSnapshot());
    if (s && s->status == SnapshotStatus::Done) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  auto s = machineSnapshot<Ctx>(sys->root()->getSnapshot());
  std::printf("finished in %s after %d attempts, user: %s\n",
              s->value.toString().c_str(), s->context.attempts, s->context.user.c_str());
  sys->stop();
  return 0;
}
