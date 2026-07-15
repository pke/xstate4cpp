#pragma once

#include <stdexcept>

namespace xstate {

// Configuration / programmer errors: thrown at createMachine or API misuse.
struct ConfigError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Malformed JSON input (machine configs, snapshots).
struct JsonError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

}  // namespace xstate
