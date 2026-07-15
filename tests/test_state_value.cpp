#include "doctest.h"
#include <xstate/state_value.hpp>
using namespace xstate;

TEST_CASE("leaf matching") {
  StateValue v{"idle"};
  CHECK(v.contains(StateValue::fromPath("idle")));
  CHECK_FALSE(v.contains(StateValue::fromPath("busy")));
}

TEST_CASE("nested and partial-path matching") {
  auto v = StateValue::fromPath("auth.card.waiting");  // {auth:{card:"waiting"}}
  CHECK(v.contains(StateValue::fromPath("auth")));
  CHECK(v.contains(StateValue::fromPath("auth.card")));
  CHECK(v.contains(StateValue::fromPath("auth.card.waiting")));
  CHECK_FALSE(v.contains(StateValue::fromPath("auth.card.done")));
  CHECK_FALSE(v.contains(StateValue::fromPath("other")));
}

TEST_CASE("parallel state value") {
  auto v = StateValue::branchesOf({{"io", StateValue{"reading"}}, {"ui", StateValue{"prompt"}}});
  CHECK(v.contains(StateValue::fromPath("io.reading")));
  CHECK(v.contains(StateValue::fromPath("ui")));
  CHECK(v.toString() == "{io: reading, ui: prompt}");
}
