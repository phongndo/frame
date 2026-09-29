#ifndef FRAME_CLIENT_ATTACHED_CLIENT_HPP
#define FRAME_CLIENT_ATTACHED_CLIENT_HPP

#include "daemon/server.hpp"

#include <string_view>

namespace frame::client {

[[nodiscard]] auto attach(const daemon::RuntimeEndpoint& endpoint, std::string_view session) -> int;

} // namespace frame::client

#endif // FRAME_CLIENT_ATTACHED_CLIENT_HPP
