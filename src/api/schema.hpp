#ifndef FRAME_API_SCHEMA_HPP
#define FRAME_API_SCHEMA_HPP

#include <string_view>

namespace frame::api {

[[nodiscard]] auto schema_document() noexcept -> std::string_view;

} // namespace frame::api

#endif // FRAME_API_SCHEMA_HPP
