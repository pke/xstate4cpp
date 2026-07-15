#include "doctest.h"
#include <xstate/machine/machine.hpp>
using namespace xstate;

namespace {
struct Ctx {
  std::vector<std::string> log{};
};

ActionRef<Ctx> mark(const char* s) {
  return assign<Ctx>([s](Ctx& x, const Event&) { x.log.push_back(s); });
}

MachineConfig<Ctx> nested() {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].initial = "a1";
  c.states["a"].entry.push_back(mark("enter.a"));
  c.states["a"].exit.push_back(mark("exit.a"));
  c.states["a"].states["a1"].entry.push_back(mark("enter.a1"));
  c.states["a"].states["a1"].exit.push_back(mark("exit.a1"));
  c.states["a"].states["a2"].entry.push_back(mark("enter.a2"));
  c.states["a"].states["a2"].exit.push_back(mark("exit.a2"));
  c.states["a"].states["a1"].on["NEXT"] = "a2";  // sibling: stays inside a
  c.states["a"].on["LEAVE"] = "b";               // handler on compound ancestor
  c.states["b"].entry.push_back(mark("enter.b"));
  return c;
}
}  // namespace

TEST_CASE("sibling transition inside compound does not exit the parent") {
  auto m = createMachine<Ctx>(nested());
  ActorScope s;
  auto s0 = m->getInitialSnapshot(s, {});
  auto s1 = machineSnapshot<Ctx>(m->transition(s0, Event{"NEXT"}, s));
  CHECK(s1->matches("a.a2"));
  // initial entry logged enter.a, enter.a1; the NEXT microstep must add only exit.a1, enter.a2
  CHECK(s1->context.log == std::vector<std::string>{"enter.a", "enter.a1", "exit.a1", "enter.a2"});
}

TEST_CASE("ancestor handles event not handled by leaf; exits deepest-first") {
  auto m = createMachine<Ctx>(nested());
  ActorScope s;
  auto s1 = machineSnapshot<Ctx>(m->transition(m->getInitialSnapshot(s, {}), Event{"LEAVE"}, s));
  CHECK(s1->matches("b"));
  CHECK(s1->context.log == std::vector<std::string>{"enter.a", "enter.a1", "exit.a1", "exit.a",
                                                    "enter.b"});
}

TEST_CASE("deeper handler overrides ancestor for same event") {
  auto c = nested();
  c.states["a"].states["a1"].on["LEAVE"] = "a2";  // leaf overrides
  auto m = createMachine<Ctx>(c);
  ActorScope s;
  auto s1 = machineSnapshot<Ctx>(m->transition(m->getInitialSnapshot(s, {}), Event{"LEAVE"}, s));
  CHECK(s1->matches("a.a2"));
}

TEST_CASE("internal vs reenter self-descendant transitions") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].initial = "a1";
  c.states["a"].entry.push_back(mark("enter.a"));
  c.states["a"].exit.push_back(mark("exit.a"));
  c.states["a"].states["a1"].entry.push_back(mark("enter.a1"));
  c.states["a"].on["RESET"] = "a.a1";  // internal: target descendant, no reenter
  auto reenterT = transition<Ctx>("a.a1");
  reenterT.reenter = true;
  c.states["a"].on["HARD_RESET"] = reenterT;
  auto m = createMachine<Ctx>(c);
  ActorScope s;
  auto s0 = m->getInitialSnapshot(s, {});
  auto soft = machineSnapshot<Ctx>(m->transition(s0, Event{"RESET"}, s));
  CHECK(soft->context.log ==
        std::vector<std::string>{"enter.a", "enter.a1", "enter.a1"});  // a not exited
  auto hard = machineSnapshot<Ctx>(m->transition(s0, Event{"HARD_RESET"}, s));
  CHECK(hard->context.log == std::vector<std::string>{"enter.a", "enter.a1", "exit.a",
                                                      "enter.a", "enter.a1"});
}
