#include "doctest.h"
#include <xstate/machine/machine.hpp>
using namespace xstate;

namespace {
struct Ctx { int n = 0; };

MachineConfig<Ctx> lightCfg() {
  MachineConfig<Ctx> c;
  c.id = "light";
  c.initial = "green";
  c.states["green"].on["TIMER"] = "yellow";
  c.states["yellow"].on["TIMER"] = "red";
  c.states["red"].id = "stop";
  c.states["red"].initial = "walk";
  c.states["red"].states["walk"].on["COUNTDOWN"] = "wait";
  c.states["red"].states["wait"];
  return c;
}
}  // namespace

TEST_CASE("parse builds resolved tree in document order") {
  auto m = createMachine<Ctx>(lightCfg());
  auto& root = m->root();
  CHECK(root.children.size() == 3);
  CHECK(root.children[0]->key == "green");
  CHECK(root.children[1]->key == "yellow");
  CHECK(root.children[2]->id == "stop");  // explicit id, document order preserved
  CHECK(root.children[0]->transitions.at(0).targets.at(0)->key == "yellow");
  CHECK(m->getNodeById("stop")->initial->key == "walk");
  CHECK(root.children[2]->children[0]->path == "red.walk");
}

TEST_CASE("unknown target throws with path") {
  auto c = lightCfg();
  c.states["green"].on["GO"] = "nowhere";
  CHECK_THROWS_WITH_AS(createMachine<Ctx>(c),
                       doctest::Contains("states.green.on.GO"), ConfigError);
}

TEST_CASE("missing initial throws") {
  MachineConfig<Ctx> c;
  c.initial = "a";
  c.states["a"].states["x"];  // compound without initial
  CHECK_THROWS_AS(createMachine<Ctx>(c), ConfigError);
}

TEST_CASE("unknown named guard/action/actor/delay throw") {
  auto c = lightCfg();
  c.states["green"].on["GO"] = transition<Ctx>("yellow").guarded("nope");
  CHECK_THROWS_WITH_AS(createMachine<Ctx>(c),
                       doctest::Contains("unknown guard 'nope'"), ConfigError);
  auto c2 = lightCfg();
  c2.states["green"].entry.push_back(action<Ctx>("nope"));
  CHECK_THROWS_AS(createMachine<Ctx>(c2), ConfigError);
  auto c3 = lightCfg();
  c3.states["green"].invoke.push_back(invoke<Ctx>("nope"));
  CHECK_THROWS_AS(createMachine<Ctx>(c3), ConfigError);
  auto c4 = lightCfg();
  c4.states["green"].after["NAMED"] = "yellow";
  CHECK_THROWS_AS(createMachine<Ctx>(c4), ConfigError);
}

TEST_CASE("#id targets resolve across the tree") {
  auto c = lightCfg();
  c.states["green"].on["PANIC"] = "#stop";
  auto m = createMachine<Ctx>(c);
  CHECK(m->root().children[0]->transitions.back().targets.at(0)->id == "stop");
}

TEST_CASE("duplicate ids and final-with-children throw") {
  auto c = lightCfg();
  c.states["yellow"].id = "stop";  // duplicate of red's id
  CHECK_THROWS_AS(createMachine<Ctx>(c), ConfigError);
  MachineConfig<Ctx> c2;
  c2.initial = "f";
  c2.states["f"].type = StateType::Final;
  c2.states["f"].states["oops"];
  CHECK_THROWS_AS(createMachine<Ctx>(c2), ConfigError);
}
