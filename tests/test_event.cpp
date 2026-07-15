#include "doctest.h"
#include <xstate/event.hpp>
using namespace xstate;

TEST_CASE("Event holds type and typed payload") {
  Event e{"FETCH", std::string("user-1")};
  CHECK(e.type == "FETCH");
  REQUIRE(e.dataAs<std::string>() != nullptr);
  CHECK(*e.dataAs<std::string>() == "user-1");
  CHECK(e.dataAs<int>() == nullptr);        // wrong type -> nullptr, no throw
  Event bare{"TOGGLE"};
  CHECK(bare.dataAs<int>() == nullptr);     // empty payload
}

TEST_CASE("reserved event builders use v5 wire format") {
  CHECK(events::init().type == "xstate.init");
  CHECK(events::doneState("m.a").type == "xstate.done.state.m.a");
  CHECK(events::doneActor("child", 42).type == "xstate.done.actor.child");
  CHECK(events::errorActor("child", 0).type == "xstate.error.actor.child");
  CHECK(events::afterEventType(1000, "light.green") == "xstate.after(1000)#light.green");
}
