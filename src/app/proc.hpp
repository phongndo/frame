#ifndef FRAME_APP_PROC_HPP
#define FRAME_APP_PROC_HPP

#include "daemon/server.hpp"

#include <string_view>

namespace frame::app {

// Parses and executes one bounded, versioned Proc document from FILE or stdin ("-").
[[nodiscard]] auto run_proc_document(const daemon::RuntimeEndpoint& endpoint,
                                     std::string_view source) -> int;

} // namespace frame::app

#endif // FRAME_APP_PROC_HPP
