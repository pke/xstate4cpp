// Initial transitions with actions (`initial: { target, actions }`).
// Upstream reference: actions.test.ts — "should execute actions of initial
// transitions only once when taking an explicit transition".
#include "doctest.h"
#include <xstate/json.hpp>
#include <xstate/machine/machine.hpp>
using namespace xstate;

namespace {
struct Ctx {
  std::vector<std::string> log{};
};

ActionRef<Ctx> mark(std::string s) {
  return assign<Ctx>([s = std::move(s)](Ctx& x, const Event&) { x.log.push_back(s); });
}

using Snap = std::shared_ptr<const MachineSnapshot<Ctx>>;

struct Rig {
  std::shared_ptr<Machine<Ctx>> m;
  ActorScope scope;
  SnapshotPtr snap;
  explicit Rig(std::shared_ptr<Machine<Ctx>> machine) : m(std::move(machine)) {
    snap = m->getInitialSnapshot(scope, {});
  }
  Snap send(Event e) {
    snap = m->transition(snap, std::move(e), scope);
    return machineSnapshot<Ctx>(snap);
  }
  Snap state() const { return machineSnapshot<Ctx>(snap); }
};

MachineConfig<Ctx> nestedInitialMachine() {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].on["NEXT"] = "b";
  auto& b = c.states["b"];
  b.initial = "b_child";
  b.initialActions.push_back(mark("initial in b"));
  auto& bChild = b.states["b_child"];
  bChild.initial = "b_grandchild";
  bChild.initialActions.push_back(mark("initial in b_child"));
  bChild.states["b_grandchild"];
  return c;
}
}  // namespace

// actions.test.ts: "should execute actions of initial transitions only once
// when taking an explicit transition"
TEST_CASE("initial actions run exactly once, outer before inner") {
  Rig r(createMachine<Ctx>(nestedInitialMachine()));
  auto s = r.send({"NEXT"});
  CHECK(s->matches("b.b_child.b_grandchild"));
  CHECK(s->context.log ==
        std::vector<std::string>{"initial in b", "initial in b_child"});
}

TEST_CASE("initial actions interleave with entry actions, parent-first") {
  auto c = nestedInitialMachine();
  c.states["b"].entry.push_back(mark("enter b"));
  c.states["b"].states["b_child"].entry.push_back(mark("enter b_child"));
  c.states["b"].states["b_child"].states["b_grandchild"].entry.push_back(
      mark("enter b_grandchild"));
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"NEXT"});
  CHECK(s->context.log ==
        std::vector<std::string>{"enter b", "initial in b", "enter b_child",
                                 "initial in b_child", "enter b_grandchild"});
}

TEST_CASE("targeting a child directly skips the parent's initial actions") {
  auto c = nestedInitialMachine();
  c.states["b"].states["b_child"].id = "bc";
  c.states["a"].on["DIRECT"] = "#bc";
  Rig r(createMachine<Ctx>(c));
  auto s = r.send({"DIRECT"});
  CHECK(s->matches("b.b_child.b_grandchild"));
  // b's initial transition was NOT taken; b_child's was (for the grandchild)
  CHECK(s->context.log == std::vector<std::string>{"initial in b_child"});
}

TEST_CASE("machine start takes all initial transitions") {
  auto c = nestedInitialMachine();
  c.initialActions.push_back(mark("initial in root"));
  c.states["a"].on["NEXT"];  // keep 'a' as initial; root initial targets a
  Rig r(createMachine<Ctx>(c));
  CHECK(r.state()->context.log == std::vector<std::string>{"initial in root"});
}

TEST_CASE("re-entering the parent re-runs its initial actions") {
  auto c = nestedInitialMachine();
  c.states["b"].on["RESTART"] = "a";
  Rig r(createMachine<Ctx>(c));
  r.send({"NEXT"});
  r.send({"RESTART"});
  auto s = r.send({"NEXT"});
  CHECK(s->context.log ==
        std::vector<std::string>{"initial in b", "initial in b_child", "initial in b",
                                 "initial in b_child"});
}

TEST_CASE("named initial actions are validated against options") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].initial = "a1";
  c.states["a"].initialActions.push_back(action<Ctx>("nope"));
  c.states["a"].states["a1"];
  CHECK_THROWS_WITH_AS(createMachine<Ctx>(c), doctest::Contains("unknown action 'nope'"),
                       ConfigError);
}

TEST_CASE("JSON initial object form maps target and actions") {
  const char* json = R"({
    "initial": "a",
    "states": {
      "a": { "on": { "NEXT": "b" } },
      "b": {
        "initial": { "target": "b_child", "actions": [ "notify" ] },
        "states": { "b_child": {} }
      }
    }
  })";
  auto cfg = parseMachineJson<Ctx>(json);
  CHECK(cfg.states["b"].initial == "b_child");
  REQUIRE(cfg.states["b"].initialActions.size() == 1);
  CHECK(cfg.states["b"].initialActions.at(0).name == "notify");

  MachineOptions<Ctx> o;
  o.actions["notify"] = [](Ctx& x, const Event&) { x.log.push_back("notified"); };
  Rig r(createMachine<Ctx>(cfg, o));
  CHECK(r.send({"NEXT"})->context.log == std::vector<std::string>{"notified"});
}
