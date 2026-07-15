// Protocol-session demo shaped like an NFC flow:
//   idle -> detecting -> session(parallel: io || ui) -> done
// with a session timeout, an invoked reader actor (fromCallback), and an
// inspection handler printing xstate wire-format JSON.
#include <chrono>
#include <cstdio>
#include <thread>

#include <xstate/xstate.hpp>
#include "example_loop.hpp"

struct Ctx {
  std::string cardId;
};

int main() {
  using namespace xstate;

  MachineConfig<Ctx> c;
  c.id = "nfc";
  c.initial = "idle";
  c.states["idle"].on["START"] = "detecting";

  auto& detecting = c.states["detecting"];
  detecting.invoke.push_back(invoke<Ctx>("reader").id("reader"));
  detecting.on["CARD_DETECTED"] = transition<Ctx>("session").act(
      assign<Ctx>([](Ctx& x, const Event& e) { x.cardId = *e.dataAs<std::string>(); }));
  detecting.after[10'000] = "idle";  // detection timeout

  auto& session = c.states["session"];
  session.type = StateType::Parallel;
  session.id = "session";
  session.states["io"].initial = "exchanging";
  session.states["io"].states["exchanging"].on["IO_DONE"] = "finished";
  session.states["io"].states["finished"].type = StateType::Final;
  session.states["ui"].initial = "prompting";
  session.states["ui"].states["prompting"].on["USER_ACK"] = "acked";
  session.states["ui"].states["acked"].type = StateType::Final;
  session.onDone.push_back(transition<Ctx>("#nfc-done"));
  session.after[10'000] = "idle";  // session timeout

  c.states["done"].id = "nfc-done";
  c.states["done"].type = StateType::Final;

  MachineOptions<Ctx> o;
  o.actors["reader"] = [](const std::any&) -> std::shared_ptr<ActorLogic> {
    return fromCallback([](CallbackHandle h, const std::any&) {
      // simulate the NFC hardware announcing a card from its own thread
      auto sendBack = h.sendBack;
      std::thread([sendBack] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        sendBack(Event{"CARD_DETECTED", std::string("04:A2:19:66")});
      }).detach();
      return [] { std::puts("reader: field off"); };
    });
  };

  exloop::EventLoop loop;
  SystemOptions so;
  so.executor = &loop;
  so.clock = &loop;
  auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c, o), so);
  sys->inspect([](const InspectionEvent& ev) { std::puts(ev.toJson().c_str()); });

  sys->root()->start();
  sys->root()->send({"START"});
  std::this_thread::sleep_for(std::chrono::milliseconds(120));  // card appears
  sys->root()->send({"IO_DONE"});
  sys->root()->send({"USER_ACK"});

  for (int i = 0; i < 100; ++i) {
    auto s = machineSnapshot<Ctx>(sys->root()->getSnapshot());
    if (s && s->status == SnapshotStatus::Done) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  auto s = machineSnapshot<Ctx>(sys->root()->getSnapshot());
  std::printf("session complete, card %s, state %s\n", s->context.cardId.c_str(),
              s->value.toString().c_str());
  sys->stop();
  return 0;
}
