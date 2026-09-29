#include "extension/lua_host.hpp"

#include <string_view>

int main(const int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  // The private launcher supplies exactly PATH and required/optional.
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const std::string_view mode(argv[2]);
  if (mode != "required" && mode != "optional") {
    return 2;
  }
  return frame::extension::run_configuration_host(argv[1], mode == "required");
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}
