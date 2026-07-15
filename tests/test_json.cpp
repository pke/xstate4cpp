#include "doctest.h"
#include <xstate/json.hpp>
#include <xstate/machine/machine.hpp>
using namespace xstate;

namespace {
struct Ctx { int n = 0; };

const char* kLightJson = R"({
  "id": "light",
  "initial": "green",
  "states": {
    "green":  { "on": { "TIMER": "yellow" } },
    "yellow": { "on": { "TIMER": "red" } },
    "red": {
      "id": "stop",
      "initial": "walk",
      "states": {
        "walk": { "on": { "COUNTDOWN": "wait" } },
        "wait": {}
      }
    }
  }
})";
}  // namespace

TEST_CASE("parses a Stately-style export into the same structure as hand-built config") {
  auto cfg = parseMachineJson<Ctx>(kLightJson);
  CHECK(cfg.id == "light");
  CHECK(cfg.initial == "green");
  CHECK(cfg.states.size() == 3);
  CHECK(cfg.states["green"].on["TIMER"].list.at(0).targets == std::vector<std::string>{"yellow"});
  CHECK(cfg.states["red"].id == "stop");
  CHECK(cfg.states["red"].initial == "walk");
  CHECK(cfg.states["red"].states["walk"].on["COUNTDOWN"].list.at(0).targets ==
        std::vector<std::string>{"wait"});
}

TEST_CASE("machine from JSON transitions like the hand-built one") {
  auto m = createMachine<Ctx>(parseMachineJson<Ctx>(kLightJson));
  ActorScope s;
  auto s1 = m->transition(m->getInitialSnapshot(s, {}), Event{"TIMER"}, s);
  auto s2 = machineSnapshot<Ctx>(m->transition(s1, Event{"TIMER"}, s));
  CHECK(s2->matches("red.walk"));
}

TEST_CASE("guard objects map to the guard algebra; transitions carry guards/actions/reenter") {
  const char* json = R"({
    "initial": "a",
    "states": {
      "a": {
        "on": {
          "GO": [
            { "target": "b",
              "guard": { "type": "xstate.guard.not", "guards": [ { "type": "blocked" } ] },
              "actions": [ "cleanup", { "type": "xstate.log", "message": "moving" } ],
              "reenter": true }
          ]
        }
      },
      "b": { "type": "final" }
    }
  })";
  auto cfg = parseMachineJson<Ctx>(json);
  const auto& t = cfg.states["a"].on["GO"].list.at(0);
  REQUIRE(t.guard.has_value());
  CHECK(t.guard->kind == GuardRef<Ctx>::Kind::Not);
  CHECK(t.guard->operands.at(0).name == "blocked");
  CHECK(t.reenter);
  REQUIRE(t.actions.size() == 2);
  CHECK(t.actions.at(0).kind == ActionRef<Ctx>::Kind::Named);
  CHECK(t.actions.at(1).kind == ActionRef<Ctx>::Kind::Log);
  CHECK(t.actions.at(1).message == "moving");
  CHECK(cfg.states["b"].type == StateType::Final);
}

TEST_CASE("after, always, invoke, entry/exit, tags, history all map") {
  const char* json = R"({
    "initial": "work",
    "states": {
      "work": {
        "entry": "notify",
        "exit": [ { "type": "xstate.cancel", "sendId": "t1" } ],
        "tags": [ "busy" ],
        "after": { "5000": "timeout", "BACKOFF": "retry" },
        "always": [ { "target": "done", "guard": "finished" } ],
        "invoke": { "src": "worker", "id": "w",
                    "onDone": "done", "onError": "failed" }
      },
      "hist": { "type": "history", "history": "deep", "target": "work" },
      "timeout": {}, "retry": {}, "failed": {}, "done": { "type": "final" }
    }
  })";
  auto cfg = parseMachineJson<Ctx>(json);
  auto& work = cfg.states["work"];
  CHECK(work.entry.at(0).name == "notify");
  CHECK(work.exit.at(0).kind == ActionRef<Ctx>::Kind::Cancel);
  CHECK(work.exit.at(0).name == "t1");
  CHECK(work.tags == std::vector<std::string>{"busy"});
  CHECK(work.after.entries.count("5000") == 1);
  CHECK(work.after.entries.count("BACKOFF") == 1);
  CHECK(work.always.at(0).guard->name == "finished");
  REQUIRE(work.invoke.size() == 1);
  CHECK(work.invoke.at(0).src == "worker");
  CHECK(work.invoke.at(0).id == "w");
  CHECK(work.invoke.at(0).onDone.at(0).targets == std::vector<std::string>{"done"});
  CHECK(cfg.states["hist"].type == StateType::History);
  CHECK(cfg.states["hist"].history == HistoryType::Deep);
  CHECK(cfg.states["hist"].target == "work");
}

TEST_CASE("malformed JSON throws JsonError with position") {
  CHECK_THROWS_WITH_AS(parseMachineJson<Ctx>(R"({"initial": })"),
                       doctest::Contains("line 1"), JsonError);
  CHECK_THROWS_AS(parseMachineJson<Ctx>(R"({"states": { "a": { "type": "xstate.nope" } } })"),
                  JsonError);  // unknown state type is also a JsonError
}

TEST_CASE("unknown xstate.* action type throws JsonError at parse") {
  CHECK_THROWS_AS(
      parseMachineJson<Ctx>(
          R"({"initial":"a","states":{"a":{"entry":{"type":"xstate.teleport"}}}})"),
      JsonError);
}

TEST_CASE("unbound named guard from JSON throws ConfigError at createMachine, not parse") {
  const char* json = R"({
    "initial": "a",
    "states": {
      "a": { "on": { "GO": { "target": "b", "guard": "nope" } } },
      "b": {}
    }
  })";
  auto cfg = parseMachineJson<Ctx>(json);  // parses fine
  CHECK_THROWS_AS(createMachine<Ctx>(cfg), ConfigError);
}
