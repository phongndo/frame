#ifndef FRAME_API_PROC_HPP
#define FRAME_API_PROC_HPP

#include <cstddef>
#include <string_view>

namespace frame::api {

inline constexpr std::string_view proc_schema = "frame.proc/v1";
inline constexpr std::string_view proc_result_schema = "frame.proc-result/v1";
inline constexpr std::size_t proc_commands_max = 64;

} // namespace frame::api

#endif // FRAME_API_PROC_HPP
