#pragma once
// Shared example helper: pick the native event loop for this platform.
#ifdef _WIN32
#include <xstate/adapters/win32/event_loop.hpp>
namespace exloop = xstate::win32;
#else
#include <xstate/adapters/posix/event_loop.hpp>
namespace exloop = xstate::posix;
#endif
