#include "doctest.h"
#include <xstate/machine/config.hpp>
using namespace xstate;

namespace {
struct Ctx { int retries = 0; };
}

TEST_CASE("config builds like the TS original") {
  MachineConfig<Ctx> cfg;
  cfg.id = "fetch";
  cfg.initial = "idle";
  cfg.states["idle"].on["FETCH"] = "loading";                  // string assign
  cfg.states["loading"].after[10000] = "failure";              // numeric delay key
  cfg.states["loading"].after["SHORT"] = "idle";               // named delay key
  cfg.states["loading"].on["CANCEL"] =
      transition<Ctx>("idle").guarded("canCancel").act("cleanup");
  cfg.states["loading"].invoke.push_back(
      invoke<Ctx>("fetchUser").onDone("success").onError("failure"));
  cfg.states["failure"].on["RETRY"].add(
      transition<Ctx>("loading").guarded(not_<Ctx>("maxRetries")));
  cfg.states["success"].type = StateType::Final;

  CHECK(cfg.states["idle"].on["FETCH"].list.at(0).targets == std::vector<std::string>{"loading"});
  CHECK(cfg.states["loading"].after.entries.count("10000") == 1);
  CHECK(cfg.states["loading"].invoke.at(0).id == "");          // defaulted at parse, not here
  CHECK(cfg.states["failure"].on["RETRY"].list.at(0).guard->kind == GuardRef<Ctx>::Kind::Not);
}

TEST_CASE("guard algebra composes") {
  auto g = and_<Ctx>({guardNamed<Ctx>("a"),
                      or_<Ctx>({guardNamed<Ctx>("b"), stateIn<Ctx>("s.t")})});
  CHECK(g.kind == GuardRef<Ctx>::Kind::And);
  CHECK(g.operands.at(1).operands.at(1).kind == GuardRef<Ctx>::Kind::StateIn);
}

TEST_CASE("action factories fill descriptions") {
  auto a = sendTo<Ctx>("logger", Event{"LOG"}, 250, "log-1");
  CHECK(a.kind == ActionRef<Ctx>::Kind::SendTo);
  CHECK(a.name == "logger");
  CHECK(a.delayMs == 250);
  CHECK(a.sendId == "log-1");
}
