#ifndef FRAME_APP_APPLICATION_HPP
#define FRAME_APP_APPLICATION_HPP

#include "daemon/server.hpp"

namespace frame::app {

// Owns command-line parsing and dispatch. Process main selects the production endpoint; test
// drivers inject a fixture-owned endpoint while exercising the same parser and components.
[[nodiscard]] auto run(const daemon::RuntimeEndpoint& endpoint, int argument_count,
                       char** argument_values) -> int;

// Test-only harness for characterizing removed tab/pane commands and private development commands.
[[nodiscard]] auto run_legacy(const daemon::RuntimeEndpoint& endpoint, int argument_count,
                              char** argument_values) -> int;

} // namespace frame::app

#endif // FRAME_APP_APPLICATION_HPP
