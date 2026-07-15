#include "doctest.h"
#include <xstate/machine/machine.hpp>
using namespace xstate;

namespace {
struct Ctx {
  int count = 0;
  std::vector<std::string> log{};
};

std::shared_ptr<Machine<Ctx>> toggle() {
  MachineConfig<Ctx> c;
  c.initial = "inactive";
  c.states["inactive"].exit.push_back(
      assign<Ctx>([](Ctx& x, const Event&) { x.log.push_back("exit.inactive"); }));
  c.states["inactive"].on["TOGGLE"] = transition<Ctx>("active").act(
      assign<Ctx>([](Ctx& x, const Event&) { x.log.push_back("action"); }));
  c.states["active"].entry.push_back(
      assign<Ctx>([](Ctx& x, const Event&) { x.log.push_back("enter.active"); }));
  c.states["active"].on["TOGGLE"] = "inactive";
  return createMachine<Ctx>(c);
}
}  // namespace

TEST_CASE("basic transition with exit->transition->entry action order") {
  auto m = toggle();
  ActorScope s;
  auto s0 = m->getInitialSnapshot(s, {});
  auto s1 = machineSnapshot<Ctx>(m->transition(s0, Event{"TOGGLE"}, s));
  CHECK(s1->matches("active"));
  CHECK(s1->context.log ==
        std::vector<std::string>{"exit.inactive", "action", "enter.active"});
}

TEST_CASE("unhandled event returns the identical snapshot") {
  auto m = toggle();
  ActorScope s;
  auto s0 = m->getInitialSnapshot(s, {});
  CHECK(m->transition(s0, Event{"UNKNOWN"}, s) == s0);  // pointer-equal
}

TEST_CASE("guards select first passing candidate in definition order") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].on["GO"].add(transition<Ctx>("b").guarded("no"))
                        .add(transition<Ctx>("c").guarded("yes"))
                        .add(transition<Ctx>("d"));
  c.states["b"]; c.states["c"]; c.states["d"];
  MachineOptions<Ctx> o;
  o.guards["no"] = [](const Ctx&, const Event&) { return false; };
  o.guards["yes"] = [](const Ctx&, const Event&) { return true; };
  auto m = createMachine<Ctx>(c, o);
  ActorScope s;
  auto s1 = machineSnapshot<Ctx>(m->transition(m->getInitialSnapshot(s, {}), Event{"GO"}, s));
  CHECK(s1->matches("c"));
}

TEST_CASE("guard algebra evaluates") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].on["GO"] = transition<Ctx>("b").guarded(
      and_<Ctx>({guardFn<Ctx>([](const Ctx& x, const Event&) { return x.count == 0; }),
                 not_<Ctx>("blocked")}));
  c.states["b"];
  MachineOptions<Ctx> o;
  o.guards["blocked"] = [](const Ctx&, const Event&) { return false; };
  auto m = createMachine<Ctx>(c, o);
  ActorScope s;
  CHECK(machineSnapshot<Ctx>(m->transition(m->getInitialSnapshot(s, {}), Event{"GO"}, s))
            ->matches("b"));
}

TEST_CASE("targetless transition runs actions without exit/entry") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].entry.push_back(assign<Ctx>([](Ctx& x, const Event&) { x.count += 10; }));
  c.states["a"].on["BUMP"] =
      transition<Ctx>().act(assign<Ctx>([](Ctx& x, const Event&) { x.count++; }));
  auto m = createMachine<Ctx>(c);
  ActorScope s;
  auto s1 = machineSnapshot<Ctx>(m->transition(m->getInitialSnapshot(s, {}), Event{"BUMP"}, s));
  CHECK(s1->matches("a"));
  CHECK(s1->context.count == 11);  // entry NOT re-run
}

TEST_CASE("wildcard handles any event; can() reports correctly") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].on["*"] = "b";
  c.states["b"];
  auto m = createMachine<Ctx>(c);
  ActorScope s;
  auto s0 = machineSnapshot<Ctx>(m->getInitialSnapshot(s, {}));
  CHECK(s0->can(Event{"ANYTHING"}));
  CHECK(machineSnapshot<Ctx>(m->transition(m->getInitialSnapshot(s, {}), Event{"X"}, s))
            ->matches("b"));
}
