# xstate4cpp

[![CI](https://github.com/pke/xstate4cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/pke/xstate4cpp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A portable, **zero-dependency, header-only C++17** port of [XState v5](https://stately.ai/docs)
— statecharts and the actor model, with the same concepts, semantics, and wire
formats as the JS/TS original.

- **Zero dependencies**: the core uses only the C++17 standard library — no
  `<thread>`, no `<mutex>`, no OS headers. All platform code lives behind two
  tiny interfaces (`Executor`, `Clock`) with **Win32**, **POSIX**, and
  **manual** (caller-pumped) adapters.
- **Same separation of concerns as XState v5**: pure machine logic →
  behavior-agnostic actors → actor system → pluggable scheduling.
- **JS-event-loop guarantees**: one executor per actor system; every
  transition runs serialized with run-to-completion — user code never needs a
  lock. `send()` and `getSnapshot()` are safe from any thread.

## Feature overview

| XState v5 | xstate4cpp |
|---|---|
| `createMachine(config)` | `createMachine<Ctx>(config, options)` |
| Hierarchical / parallel / final / history states | ✅ |
| Guards incl. `and`/`or`/`not`/`stateIn` | ✅ |
| `entry`/`exit`/transition actions, `assign` | ✅ (actions mutate a draft context) |
| `raise`, `sendTo`, `sendParent`, `log`, `cancel`, `stopChild`, `spawnChild`, `emit` | ✅ |
| Eventless (`always`) transitions, wildcard events | ✅ |
| Delayed transitions (`after`), named delays | ✅ via `Clock` |
| `invoke` with `onDone` / `onError` | ✅ |
| `createActor` / actor system / receptionist (`systemId`) | ✅ |
| `fromPromise` / `fromCallback` / `fromTransition` | `fromAsync` / `fromCallback` / `fromTransition` |
| Persistence (`getPersistedSnapshot` / restore) | ✅ structured + JSON forms |
| Inspection API (`@xstate.actor/event/snapshot`) | ✅ |
| JSON machine import (Stately Studio exports) | ✅ optional `xstate/json.hpp` |
| `fromObservable`, typed facade | out of scope (v1) |

## 30-second example

```cpp
#include <xstate/xstate.hpp>
#include <xstate/adapters/posix/event_loop.hpp>   // or adapters/win32/event_loop.hpp

struct Ctx { int toggles = 0; };

int main() {
  using namespace xstate;

  MachineConfig<Ctx> c;
  c.initial = "inactive";
  c.states["inactive"].on["TOGGLE"] =
      transition<Ctx>("active").act(assign<Ctx>([](Ctx& x, const Event&) { x.toggles++; }));
  c.states["active"].on["TOGGLE"] = "inactive";
  c.states["active"].after[5000] = "inactive";    // auto-off after 5s

  posix::EventLoop loop;                          // Executor + Clock in one thread
  SystemOptions o; o.executor = &loop; o.clock = &loop;
  auto sys = createActorSystem<Ctx>(createMachine<Ctx>(c), o);

  sys->root()->subscribe([](SnapshotPtr s) { /* runs on the loop thread */ });
  sys->root()->start();
  sys->root()->send({"TOGGLE"});                  // thread-safe from anywhere
  // ...
  sys->stop();                                    // loop dtor drains + joins
}
```

## Choosing an adapter

| Adapter | Use when |
|---|---|
| `posix::EventLoop` | Linux/macOS/iOS/Android — one pthread runs the whole system |
| `win32::EventLoop` | Windows — same, on a Win32 thread |
| `manual::ManualExecutor` + `manual::TestClock` | you own the loop (UI thread) or want deterministic tests: `pump()` runs pending work, `advance(ms)` fires timers virtually |

Lifetime rule: the loop must outlive the system — declare it first.
Custom adapters implement two virtuals: `Executor::post(Task)` and
`Clock::setTimeout/clearTimeout`.

## JSON machines (Stately Studio interop)

```cpp
#include <xstate/json.hpp>
auto cfg = xstate::parseMachineJson<Ctx>(jsonExportedFromStately);
MachineOptions<Ctx> opts;                    // bind the named references
opts.guards["isValid"] = [](const Ctx&, const Event&) { return true; };
opts.actors["fetchUser"] = ...;
auto machine = xstate::createMachine<Ctx>(cfg, opts);  // unbound names throw here
```

JSON carries *named references only* (same limitation as XState's own
serialized configs) — implementations always bind through `MachineOptions`.

## Persistence

```cpp
std::any snap = actor->getPersistedSnapshot();        // full tree, any copyable Ctx
ActorOptions restore; restore.snapshot = snap;
auto sys2 = createActorSystem<Ctx>(machine, opts2, restore);

// JSON form (needs a context codec; v1: no live children)
XSTATE_CONTEXT_FIELDS(Ctx, toggles)
machine->setContextCodec(xstateCodecFor(static_cast<Ctx*>(nullptr)));
std::string json = actor->getPersistedSnapshotJson();
restore.snapshot = machine->parseSnapshotJson(json);
```

`after` timers restart from their full delay on restore; history and child
actors round-trip in the structured form.

## Inspection

```cpp
sys->inspect([](const xstate::InspectionEvent& ev) {
  log(ev.toJson());   // {"type":"@xstate.event","sessionId":"x:0","event":{...}}
});
```

## Building / embedding

Header-only. Either:

```cmake
add_subdirectory(xstate4cpp)          # provides xstate::xstate INTERFACE target
target_link_libraries(app PRIVATE xstate::xstate)
```

or just add `-I xstate4cpp/include` to any build system (Gradle NDK, Xcode, make).

Tests and examples (dev-only; doctest is vendored, builds offline):

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

Requires CMake ≥ 3.16 and any C++17 compiler (MSVC 2017+, GCC 7+, Clang 5+).

## Errors

- **Config/programmer errors throw**: `createMachine` validates everything up
  front (`ConfigError` with the config path), malformed JSON throws `JsonError`.
- **Runtime errors become events**: an exception thrown in a guard/action/actor
  is caught at the actor boundary — invoked children route it to `onError`
  (`xstate.error.actor.<id>`), otherwise the actor's status becomes `Error`.
  The event-loop thread never dies from a user exception.

## License

[MIT](LICENSE) — same as XState itself.
