#include "doctest.h"
#include <xstate/actor.hpp>
#include <xstate/system.hpp>
#include <xstate/machine/machine.hpp>
#include <xstate/logic/async.hpp>
#include <xstate/logic/callback.hpp>
#include <xstate/logic/transition.hpp>
#include <xstate/adapters/manual/manual_executor.hpp>
#include <xstate/adapters/manual/test_clock.hpp>
#include <optional>
using namespace xstate;

namespace {
struct PCtx {
  std::string result;
  std::string received;
};

struct Rig {
  manual::ManualExecutor ex;
  manual::TestClock clk;
  std::shared_ptr<ActorSystem> sys;
  explicit Rig(std::shared_ptr<ActorLogic> logic) {
    SystemOptions o;
    o.executor = &ex;
    o.clock = &clk;
    sys = ActorSystem::create(std::move(logic), o);
    sys->root()->start();
    ex.pump();
  }
};
}  // namespace

TEST_CASE("fromTransition: plain reducer over events") {
  auto logic = fromTransition<int>([](int n, const Event& e) {
    if (e.type == "INC") return n + 1;
    if (e.type == "DEC") return n - 1;
    return n;
  }, 10);
  Rig r(logic);
  REQUIRE(transitionState<int>(r.sys->root()->getSnapshot()) != nullptr);
  CHECK(*transitionState<int>(r.sys->root()->getSnapshot()) == 10);
  r.sys->root()->send({"INC"});
  r.sys->root()->send({"INC"});
  r.sys->root()->send({"DEC"});
  r.ex.pump();
  CHECK(*transitionState<int>(r.sys->root()->getSnapshot()) == 11);
}

TEST_CASE("fromAsync: resolve/reject from outside the loop complete the invoke") {
  std::optional<Resolver> pending;
  auto makeParent = [&]() {
    MachineConfig<PCtx> c;
    c.initial = "running";
    c.states["running"].invoke.push_back(
        invoke<PCtx>("worker").id("w")
            .onDone(transition<PCtx>("ok").act(assign<PCtx>([](PCtx& x, const Event& e) {
              x.result = *e.dataAs<std::string>();
            })))
            .onError(transition<PCtx>("failed")));
    c.states["ok"];
    c.states["failed"];
    MachineOptions<PCtx> o;
    o.actors["worker"] = [&pending](const std::any&) -> std::shared_ptr<ActorLogic> {
      return fromAsync([&pending](Resolver r, const std::any&) { pending = r; });
    };
    return createMachine<PCtx>(c, o);
  };

  SUBCASE("resolve -> onDone with output") {
    manual::ManualExecutor ex;
    manual::TestClock clk;
    SystemOptions o;
    o.executor = &ex;
    o.clock = &clk;
    auto sys = createActorSystem<PCtx>(makeParent(), o);
    sys->root()->start();
    ex.pump();
    REQUIRE(pending.has_value());
    CHECK(machineSnapshot<PCtx>(sys->root()->getSnapshot())->matches("running"));
    pending->resolve(std::string("payload"));  // callable from any thread
    ex.pump();
    auto snap = machineSnapshot<PCtx>(sys->root()->getSnapshot());
    CHECK(snap->matches("ok"));
    CHECK(snap->context.result == "payload");
  }
  SUBCASE("reject -> onError; second outcome is ignored") {
    manual::ManualExecutor ex;
    manual::TestClock clk;
    SystemOptions o;
    o.executor = &ex;
    o.clock = &clk;
    auto sys = createActorSystem<PCtx>(makeParent(), o);
    sys->root()->start();
    ex.pump();
    pending->reject(std::string("nope"));
    pending->resolve(std::string("too late"));  // first outcome sticks
    ex.pump();
    CHECK(machineSnapshot<PCtx>(sys->root()->getSnapshot())->matches("failed"));
  }
}

TEST_CASE("fromCallback: sendBack, onReceive, cleanup on exit") {
  bool cleanedUp = false;
  std::string childReceived;
  MachineConfig<PCtx> c;
  c.initial = "listening";
  c.states["listening"].invoke.push_back(invoke<PCtx>("cb").id("cb"));
  c.states["listening"].on["FROM_CHILD"] =
      transition<PCtx>("got").act(assign<PCtx>([](PCtx& x, const Event& e) {
        x.received = *e.dataAs<std::string>();
      }));
  c.states["listening"].on["TELL_CHILD"] =
      transition<PCtx>().act(sendTo<PCtx>("cb", Event{"NUDGE", std::string("hi")}));
  c.states["got"];
  MachineOptions<PCtx> o;
  o.actors["cb"] = [&](const std::any&) -> std::shared_ptr<ActorLogic> {
    return fromCallback([&](CallbackHandle h, const std::any&) {
      h.onReceive([&childReceived](const Event& e) {
        if (e.type == "NUDGE") childReceived = *e.dataAs<std::string>();
      });
      h.sendBack(Event{"FROM_CHILD", std::string("card-detected")});
      return [&cleanedUp] { cleanedUp = true; };
    });
  };
  manual::ManualExecutor ex;
  manual::TestClock clk;
  SystemOptions so;
  so.executor = &ex;
  so.clock = &clk;
  auto sys = createActorSystem<PCtx>(createMachine<PCtx>(c, o), so);
  sys->root()->start();
  ex.pump();

  // child already sent FROM_CHILD during start
  auto snap = machineSnapshot<PCtx>(sys->root()->getSnapshot());
  CHECK(snap->matches("got"));
  CHECK(snap->context.received == "card-detected");
  CHECK(cleanedUp);  // leaving "listening" stopped the callback actor
}

TEST_CASE("fromCallback: onReceive receives sendTo events") {
  bool cleanedUp = false;
  std::string childReceived;
  MachineConfig<PCtx> c;
  c.initial = "listening";
  c.states["listening"].invoke.push_back(invoke<PCtx>("cb").id("cb"));
  c.states["listening"].on["TELL_CHILD"] =
      transition<PCtx>().act(sendTo<PCtx>("cb", Event{"NUDGE", std::string("hi")}));
  MachineOptions<PCtx> o;
  o.actors["cb"] = [&](const std::any&) -> std::shared_ptr<ActorLogic> {
    return fromCallback([&](CallbackHandle h, const std::any&) {
      h.onReceive([&childReceived](const Event& e) {
        if (e.type == "NUDGE") childReceived = *e.dataAs<std::string>();
      });
      return [&cleanedUp] { cleanedUp = true; };
    });
  };
  manual::ManualExecutor ex;
  manual::TestClock clk;
  SystemOptions so;
  so.executor = &ex;
  so.clock = &clk;
  auto sys = createActorSystem<PCtx>(createMachine<PCtx>(c, o), so);
  sys->root()->start();
  ex.pump();
  sys->root()->send({"TELL_CHILD"});
  ex.pump();
  CHECK(childReceived == "hi");
  sys->stop();
  ex.pump();
  CHECK(cleanedUp);  // system stop runs cleanups
}
