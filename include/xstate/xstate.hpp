#pragma once

// xstate4cpp — umbrella header for the core library.
//
// Deliberately NOT included (opt-in):
//   xstate/json.hpp                      — JSON machine-config import
//   xstate/adapters/posix/event_loop.hpp — POSIX pthread event loop
//   xstate/adapters/win32/event_loop.hpp — Win32 event loop
//   xstate/adapters/manual/*             — caller-pumped executor + test clock

#include "actor.hpp"
#include "actor_logic.hpp"
#include "errors.hpp"
#include "event.hpp"
#include "interfaces/clock.hpp"
#include "interfaces/executor.hpp"
#include "logic/async.hpp"
#include "logic/callback.hpp"
#include "logic/transition.hpp"
#include "machine/actions.hpp"
#include "machine/config.hpp"
#include "machine/guards.hpp"
#include "machine/machine.hpp"
#include "machine/state_node.hpp"
#include "snapshot.hpp"
#include "state_value.hpp"
#include "system.hpp"
