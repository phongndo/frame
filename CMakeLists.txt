cmake_minimum_required(VERSION 3.30)

project(frame VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
  message(FATAL_ERROR "frame requires Clang or Apple Clang")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep compiler output independent of the checkout path so ccache entries can be reused by
# worktrees. CMakePresets.json supplies the matching per-worktree CCACHE_BASEDIR.
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")

option(FRAME_BUILD_TESTS "Build the test suite" ON)
option(FRAME_BUILD_BENCHMARKS "Build benchmarks" ON)
option(FRAME_BUILD_FUZZERS "Build sanitizer-backed parser fuzz targets" OFF)
option(FRAME_ENABLE_LATENCY_TRACE "Compile the opt-in key-to-visible diagnostic trace" OFF)
option(FRAME_VALIDATE_GHOSTTY_EVERY_BUILD "Revalidate the Ghostty checkout on every build" ON)

if(FRAME_ENABLE_LATENCY_TRACE)
  add_compile_definitions(FRAME_ENABLE_LATENCY_TRACE=1)
endif()

add_library(frame_warnings INTERFACE)
target_compile_options(
  frame_warnings
  INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wcast-align
    -Wcast-qual
    -Wconversion
    -Wdocumentation
    -Wdouble-promotion
    -Wextra-semi
    -Wformat=2
    -Wimplicit-fallthrough
    -Wmissing-declarations
    -Wnon-virtual-dtor
    -Wnull-dereference
    -Wold-style-cast
    -Woverloaded-virtual
    -Wshadow-all
    -Wsign-conversion
    -Wswitch-enum
    -Wthread-safety
    -Wundef
    -Wunreachable-code-aggressive
    -Wzero-as-null-pointer-constant
)

# Conan selects Linux's libstdc++ ABI with -stdlib=libstdc++. The Nix Clang wrapper reports the
# otherwise valid default-library selection as unused, so suppress that driver-only diagnostic.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_options(
    frame_warnings
    INTERFACE -Wno-unused-command-line-argument
  )
  # Ghostty's Zig-produced archive contains assembly objects without GNU-stack notes. Make the
  # intended non-executable stack explicit instead of relying on the linker's deprecated fallback.
  add_link_options("LINKER:-z,noexecstack")
endif()

# libghostty-vt remains pinned, private, and built without generated output in the submodule.
include(cmake/GhosttyVt.cmake)
frame_add_pinned_ghostty()

find_package(zstd CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(Lua 5.4 REQUIRED)
if(TARGET lua::lua)
  set(FRAME_LUA_TARGET lua::lua)
elseif(TARGET Lua::Lua)
  set(FRAME_LUA_TARGET Lua::Lua)
else()
  add_library(frame_lua_external INTERFACE)
  target_include_directories(frame_lua_external SYSTEM INTERFACE "${LUA_INCLUDE_DIR}")
  target_link_libraries(frame_lua_external INTERFACE ${LUA_LIBRARIES})
  set(FRAME_LUA_TARGET frame_lua_external)
endif()

function(frame_internal_target target)
  target_include_directories(
    ${target}
    PUBLIC
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
  )
  target_link_libraries(${target} PRIVATE frame_warnings)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Independent helpers should map only the C++ runtime they use. Per-function sections let
    # the linker discard unused runtime and application code from Release executables.
    target_compile_options(${target} PRIVATE
      "$<$<CONFIG:Release>:-ffunction-sections>"
      "$<$<CONFIG:Release>:-fdata-sections>")
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "EXECUTABLE")
      target_link_options(${target} PRIVATE
        "$<$<CONFIG:Release>:-static-libstdc++>"
        "$<$<CONFIG:Release>:-static-libgcc>"
        "$<$<CONFIG:Release>:LINKER:--gc-sections,--as-needed>")
    endif()
  endif()
endfunction()

# Build and ship the same self-contained virtual-terminal description on every host.
# No ncurses library enters the native hot path.
find_program(FRAME_TIC_EXECUTABLE tic REQUIRED)
set(FRAME_TERMINFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/terminfo")
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/terminfo.stamp"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${FRAME_TERMINFO_DIR}"
  COMMAND "${FRAME_TIC_EXECUTABLE}" -x -o "${FRAME_TERMINFO_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/terminfo/frame.terminfo"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/terminfo.stamp"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/terminfo/frame.terminfo"
  VERBATIM
)
add_custom_target(frame_terminfo ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/terminfo.stamp")
install(DIRECTORY "${FRAME_TERMINFO_DIR}/" DESTINATION share/terminfo)
install(FILES terminfo/frame.terminfo DESTINATION share/frame)
install(FILES terminfo/LICENSE DESTINATION share/doc/frame/licenses RENAME ncurses.txt)
install(FILES extensions/frame_client.py extensions/picker.py DESTINATION share/frame/extensions)

add_library(frame_base STATIC src/core/assert.cpp)
frame_internal_target(frame_base)

add_library(frame_diagnostic STATIC src/diagnostic/latency_trace.cpp)
frame_internal_target(frame_diagnostic)

# This is the only target that may include Ghostty headers. The final executable receives the
# static archive as a link-only implementation requirement; Ghostty is not part of Frame's API.
add_library(frame_image STATIC src/image/png.cpp)
frame_internal_target(frame_image)
target_link_libraries(frame_image PRIVATE frame_base PNG::PNG)

add_library(
  frame_terminal STATIC
  src/terminal/terminal_core.cpp
  src/terminal/terminal_effects.cpp
  src/terminal/terminal_clipboard.cpp
  src/terminal/terminal_graphics.cpp
  src/terminal/terminal_input.cpp
  src/terminal/fingerprint.cpp
  src/terminal/terminal_render.cpp
  src/terminal/terminal_selection.cpp
  src/terminal/terminal_measure.cpp
)
frame_internal_target(frame_terminal)
target_link_libraries(frame_terminal PRIVATE frame_base frame_diagnostic frame_image ghostty-vt-static)

add_library(
  frame_platform STATIC
  src/platform/io.cpp
  src/platform/readiness.cpp
  src/platform/pty.cpp
  src/platform/terminal_mode.cpp
)
frame_internal_target(frame_platform)
add_dependencies(frame_platform frame_terminfo)

add_library(frame_protocol STATIC src/protocol/attachment.cpp)
frame_internal_target(frame_protocol)
target_link_libraries(frame_protocol PRIVATE frame_base)

add_library(frame_status STATIC src/render/status_line.cpp src/render/ui.cpp)
frame_internal_target(frame_status)
target_link_libraries(frame_status PRIVATE frame_base)

add_library(
  frame_render STATIC
  src/render/grid.cpp
  src/render/pane_composition.cpp
  src/render/graphics.cpp
  src/render/frame_buffer.cpp
)
frame_internal_target(frame_render)
target_link_libraries(
  frame_render PRIVATE frame_base frame_diagnostic frame_platform frame_terminal frame_status
)

add_library(
  frame_core STATIC
  src/core/command.cpp
  src/core/copy_mode.cpp
  src/core/float_layer.cpp
  src/core/layout.cpp
  src/core/session.cpp
  src/core/session_machine.cpp
)
frame_internal_target(frame_core)
target_link_libraries(frame_core PRIVATE frame_base)

add_library(frame_keys STATIC src/config/keys.cpp)
frame_internal_target(frame_keys)
add_executable(frame_compile_keymap src/user/compile_keymap.cpp)
frame_internal_target(frame_compile_keymap)
target_link_libraries(frame_compile_keymap PRIVATE frame_keys ${FRAME_LUA_TARGET})
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/input")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/input/default_keymap.inc"
  COMMAND frame_compile_keymap "${CMAKE_CURRENT_SOURCE_DIR}/src/user/keymap.lua"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/input/default_keymap.inc"
  DEPENDS frame_compile_keymap src/user/keymap.lua)
# Source analysis runs on fresh trees without building the application first.
add_custom_target(frame_analysis_inputs
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/input/default_keymap.inc")
add_dependencies(frame_analysis_inputs zig_build_lib_vt)
add_library(frame_input STATIC src/input/input_router.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/generated/input/default_keymap.inc")
target_include_directories(frame_input PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
frame_internal_target(frame_input)
target_link_libraries(frame_input PRIVATE frame_base)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/schema/frame-api-v1.schema.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/schema/frame-api-v1.schema.json" FRAME_API_SCHEMA)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/api/schema.cpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/api_schema.cpp"
  @ONLY
)
add_library(
  frame_api STATIC
  src/api/command.cpp
  src/api/json.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/generated/api_schema.cpp"
)
frame_internal_target(frame_api)
target_link_libraries(frame_api PRIVATE frame_base frame_core)

add_library(frame_config STATIC src/config/config.cpp)
frame_internal_target(frame_config)
target_link_libraries(frame_config PRIVATE frame_api frame_base frame_input frame_keys)

add_library(
  frame_extension_contract STATIC
  src/extension/commands.cpp
  src/extension/protocol.cpp
)
frame_internal_target(frame_extension_contract)
target_link_libraries(frame_extension_contract PRIVATE frame_api frame_platform)

add_library(frame_extension_client STATIC src/extension/client.cpp)
frame_internal_target(frame_extension_client)
target_link_libraries(frame_extension_client PRIVATE frame_api frame_extension_contract frame_platform)

# Statusline attention policy is pure user-layer state, separated so unit tests can link it.
add_library(frame_ui_attention STATIC src/user/attention.cpp)
frame_internal_target(frame_ui_attention)
target_link_libraries(frame_ui_attention PRIVATE frame_api)

add_executable(frame-ui src/user/main.cpp src/user/session_manager.cpp)
frame_internal_target(frame-ui)
target_link_libraries(frame-ui PRIVATE frame_extension_client frame_api frame_status frame_ui_attention)
install(TARGETS frame-ui RUNTIME DESTINATION bin)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/user/defaults.lua")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/user/defaults.lua" FRAME_DEFAULTS_LUA)
configure_file(src/extension/defaults.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/extension/defaults.hpp" @ONLY)
add_library(frame_extension STATIC src/extension/lua_host.cpp src/extension/lua_commands.cpp
  src/extension/external_command.cpp src/extension/services.cpp)
frame_internal_target(frame_extension)
target_include_directories(frame_extension PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(
  frame_extension PRIVATE frame_base frame_config frame_extension_contract frame_input frame_platform ${FRAME_LUA_TARGET}
)

add_executable(frame-config-host src/extension/config_host_main.cpp)
frame_internal_target(frame-config-host)
target_link_libraries(frame-config-host PRIVATE frame_extension)
install(TARGETS frame-config-host RUNTIME DESTINATION bin)

add_executable(frame-clipboard-host src/clipboard/host_main.cpp)
frame_internal_target(frame-clipboard-host)
target_link_libraries(frame-clipboard-host PRIVATE frame_image frame_platform)
install(TARGETS frame-clipboard-host RUNTIME DESTINATION bin)

add_library(
  frame_runtime STATIC
  src/core/client_frame_output.cpp
  src/core/command_history.cpp
  src/core/command_line.cpp
  src/clipboard/transaction.cpp
  src/clipboard/png_file.cpp
  src/core/engine.cpp
  src/core/engine_projection.cpp
  src/core/engine_state.cpp
  src/core/frame_scheduler.cpp
  src/core/input.cpp
  src/core/outer_attention.cpp
  src/core/pty_writer.cpp
  src/core/presentation_gate.cpp
  src/core/terminal_resize.cpp
  src/extension/runtime.cpp
)
frame_internal_target(frame_runtime)
target_link_libraries(
  frame_runtime
  PRIVATE
    frame_api
    frame_base
    frame_core
    frame_config
    frame_diagnostic
    frame_extension_contract
    frame_input
    frame_platform
    frame_protocol
    frame_render
    frame_terminal
)

# Public dependency-version helpers remain separate from semantic Core and Runtime ownership.
add_library(frame_metadata STATIC src/core/frame.cpp)
frame_internal_target(frame_metadata)
target_link_libraries(frame_metadata PRIVATE frame_terminal zstd::libzstd_static)

add_library(frame_daemon STATIC src/daemon/server.cpp)
frame_internal_target(frame_daemon)
target_link_libraries(
  frame_daemon
  PRIVATE frame_api frame_base frame_extension frame_platform frame_protocol frame_runtime
)

add_library(
  frame_client STATIC
  src/client/attached_client.cpp
  src/client/host_input_parser.cpp
  src/client/host_terminal_theme.cpp
  src/client/outer_resize_schedule.cpp
)
frame_internal_target(frame_client)
target_link_libraries(
  frame_client
  PRIVATE frame_daemon frame_diagnostic frame_platform frame_protocol
)

add_library(frame_app STATIC src/app/application.cpp src/app/proc.cpp)
frame_internal_target(frame_app)
target_link_libraries(
  frame_app PRIVATE frame_api frame_client frame_daemon frame_extension frame_platform frame_terminal
)

# Compatibility aggregate for existing tests and embedders. New internal code should depend on the
# narrow component target it uses rather than this aggregate.
add_library(frame_lib INTERFACE)
add_library(frame::lib ALIAS frame_lib)
target_link_libraries(
  frame_lib
  INTERFACE frame_core frame_input frame_metadata frame_runtime frame_terminal
)
target_include_directories(
  frame_lib
  INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:include>"
)

add_executable(frame apps/frame/main.cpp)
add_dependencies(frame frame-config-host frame-clipboard-host)
frame_internal_target(frame)
target_link_libraries(frame PRIVATE frame_app)
install(TARGETS frame RUNTIME DESTINATION bin)
add_dependencies(frame frame-ui)

if(FRAME_BUILD_TESTS OR FRAME_BUILD_BENCHMARKS)
  add_executable(frame_test_server tests/support/server_main.cpp)
  frame_internal_target(frame_test_server)
  target_link_libraries(frame_test_server PRIVATE frame_daemon)
  add_dependencies(frame_test_server frame-ui frame-config-host frame-clipboard-host)

  add_executable(frame_test_cli tests/support/cli_main.cpp)
  frame_internal_target(frame_test_cli)
  target_link_libraries(frame_test_cli PRIVATE frame_app frame_daemon frame_protocol)
  add_dependencies(frame_test_cli frame-ui frame-config-host frame-clipboard-host)

  add_executable(frame_test_pty_peer tests/support/pty_peer_main.cpp)
  target_link_libraries(frame_test_pty_peer PRIVATE frame_warnings)

  add_executable(frame_benchmark_probe benchmarks/process_probe.cpp)
  target_link_libraries(frame_benchmark_probe PRIVATE frame_warnings frame_diagnostic)
endif()

if(FRAME_BUILD_TESTS)
  enable_testing()
  add_test(NAME benchmark.probe COMMAND frame_benchmark_probe --self-test)
  find_package(GTest CONFIG REQUIRED)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME benchmark.command-probe
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/process_probe_test.py"
      "$<TARGET_FILE:frame_benchmark_probe>" "$<TARGET_FILE:frame_test_pty_peer>")
  set_tests_properties(benchmark.command-probe PROPERTIES TIMEOUT 30)
  include(GoogleTest)

  # Fast pure/component invariants. The developer entry point runs this binary once; CTest still
  # discovers individual cases for filtering and CI reporting.
  add_executable(
    frame_unit_tests
    tests/api_test.cpp
    tests/config_test.cpp
    tests/extension_commands_test.cpp
    tests/extension_protocol_test.cpp
    tests/command_history_test.cpp
    tests/command_line_test.cpp
    tests/connection_output_test.cpp
    tests/core_test.cpp
    tests/frame_scheduler_test.cpp
    tests/host_input_parser_test.cpp
    tests/host_terminal_theme_test.cpp
    tests/input_router_test.cpp
    tests/latency_trace_test.cpp
    tests/layout_test.cpp
    tests/outer_attention_test.cpp
    tests/outer_resize_schedule_test.cpp
    tests/protocol_test.cpp
    tests/reactor_environment_test.cpp
    tests/status_attention_test.cpp
  )
  frame_internal_target(frame_unit_tests)
  target_link_libraries(
    frame_unit_tests
    PRIVATE
      frame_api
      frame_client
      frame_config
      frame_core
      frame_diagnostic
      frame_extension
      frame_input
      frame_protocol
      frame_runtime
      frame_ui_attention
      GTest::gtest_main
      GTest::gmock
  )
  gtest_discover_tests(frame_unit_tests PROPERTIES LABELS "unit")

  # Frame-owned assumptions at the concrete libghostty-vt boundary. These tests do not open PTYs
  # or spawn children; expensive history/resource sweeps are separate below.
  add_executable(
    frame_terminal_boundary_tests
    tests/client_frame_output_test.cpp
    tests/clipboard_test.cpp
    tests/graphics_test.cpp
    tests/core_input_test.cpp
    tests/pty_writer_test.cpp
    tests/render_composition_test.cpp
    tests/terminal_hyperlink_test.cpp
    tests/terminal_integration_regression_test.cpp
    tests/terminal_test.cpp
  )
  frame_internal_target(frame_terminal_boundary_tests)
  target_link_libraries(
    frame_terminal_boundary_tests
    PRIVATE
      frame::lib
      frame_client
      frame_diagnostic
      frame_platform
      frame_protocol
      frame_render
      GTest::gtest_main
      GTest::gmock
  )
  gtest_discover_tests(frame_terminal_boundary_tests PROPERTIES LABELS "terminal-unit")

  # Real platform-process contracts remain native but are never presented as pure unit tests.
  add_executable(frame_component_integration_tests tests/platform_test.cpp tests/readiness_test.cpp)
  frame_internal_target(frame_component_integration_tests)
  target_link_libraries(
    frame_component_integration_tests
    PRIVATE frame::lib frame_client frame_platform frame_protocol GTest::gtest_main
  )
  gtest_discover_tests(
    frame_component_integration_tests
    PROPERTIES TIMEOUT 30 LABELS "component-integration" RUN_SERIAL TRUE
  )

  add_executable(frame_dependency_integrity_tests tests/dependency_integrity_test.cpp)
  frame_internal_target(frame_dependency_integrity_tests)
  target_link_libraries(
    frame_dependency_integrity_tests PRIVATE frame_terminal GTest::gtest_main
  )
  gtest_discover_tests(
    frame_dependency_integrity_tests PROPERTIES LABELS "dependency-terminal"
  )
  add_test(
    NAME dependency.ghostty_header_boundary
    COMMAND
      "${CMAKE_COMMAND}" "-DFRAME_SOURCE_DIR=${CMAKE_SOURCE_DIR}" -P
      "${CMAKE_SOURCE_DIR}/cmake/CheckGhosttyHeaderBoundary.cmake"
  )
  set_tests_properties(
    dependency.ghostty_header_boundary PROPERTIES LABELS "dependency;terminal"
  )

  add_executable(frame_terminal_resource_tests tests/terminal_resource_test.cpp)
  frame_internal_target(frame_terminal_resource_tests)
  target_link_libraries(frame_terminal_resource_tests PRIVATE frame_terminal GTest::gtest_main)
  gtest_discover_tests(
    frame_terminal_resource_tests
    PROPERTIES TIMEOUT 60 LABELS "extended-stress-terminal" RUN_SERIAL TRUE
  )

  add_executable(
    frame_core_sim_tests
    tests/sim/core_world_test.cpp
    tests/sim/mux_trace.cpp
    tests/sim/mux_world_test.cpp
    tests/sim/reduce_test.cpp
  )
  frame_internal_target(frame_core_sim_tests)
  target_link_libraries(frame_core_sim_tests PRIVATE frame_core GTest::gtest_main)
  gtest_discover_tests(
    frame_core_sim_tests
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    PROPERTIES TIMEOUT 30 LABELS "extended-stress-sim" RUN_SERIAL TRUE
  )

  add_executable(frame_protocol_sim_tests tests/sim/protocol_world_test.cpp)
  frame_internal_target(frame_protocol_sim_tests)
  target_link_libraries(
    frame_protocol_sim_tests PRIVATE frame_api frame_protocol GTest::gtest_main
  )
  gtest_discover_tests(
    frame_protocol_sim_tests
    PROPERTIES TIMEOUT 30 LABELS "extended-stress-sim-protocol" RUN_SERIAL TRUE
  )

  add_executable(
    frame_terminal_sim_tests
    tests/sim/composition_world_test.cpp
    tests/sim/presentation_world_test.cpp
    tests/sim/terminal_world_test.cpp
  )
  frame_internal_target(frame_terminal_sim_tests)
  target_link_libraries(
    frame_terminal_sim_tests
    PRIVATE frame_protocol frame_render frame_runtime frame_terminal GTest::gtest_main
  )
  gtest_discover_tests(
    frame_terminal_sim_tests
    PROPERTIES TIMEOUT 30 LABELS "extended-stress-sim-terminal" RUN_SERIAL TRUE
  )

  function(frame_add_python_mux_test name module)
    add_test(
      NAME "mux.${name}"
      COMMAND
        "${CMAKE_COMMAND}" -E env
        "FRAME_TEST_SERVER=$<TARGET_FILE:frame_test_server>"
        "FRAME_TEST_CLI=$<TARGET_FILE:frame_test_cli>"
        "FRAME_TEST_EXECUTABLE=$<TARGET_FILE:frame>"
        "FRAME_TEST_PTY_PEER=$<TARGET_FILE:frame_test_pty_peer>"
        "${Python3_EXECUTABLE}" -m unittest -v "${module}"
    )
    set_tests_properties(
      "mux.${name}"
      PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        TIMEOUT 45
        LABELS "mux;integration;pty"
        RUN_SERIAL TRUE
    )
  endfunction()

  frame_add_python_mux_test(configuration tests.mux.test_configuration)
  # Keep independent suites within the per-entry deadline under sanitizers and on slower runners.
  frame_add_python_mux_test(extensions.lua tests.mux.test_extensions.ExtensionMuxTest)
  frame_add_python_mux_test(extensions.picker tests.mux.test_extensions.PickerMuxTest)
  frame_add_python_mux_test(user_extensions.extensions
                            tests.mux.test_user_extensions.UserExtensionsMuxTest)
  frame_add_python_mux_test(user_extensions.session_picker
                            tests.mux.test_user_extensions.SessionPickerMuxTest)
  frame_add_python_mux_test(command_line tests.mux.test_command_line)
  frame_add_python_mux_test(pane_lifecycle tests.mux.test_pane_lifecycle)
  frame_add_python_mux_test(session_lifecycle tests.mux.test_session_lifecycle)
  frame_add_python_mux_test(resize.geometry tests.mux.test_resize.ResizeMuxTest)
  frame_add_python_mux_test(resize.copy_reflow tests.mux.test_resize.CopyModeReflowMuxTest)
  frame_add_python_mux_test(terminal_boundary.clipboard
                            tests.mux.test_terminal_boundary.ClipboardMuxTest)
  # Clipboard includes real 30-second transport and 10-second worker deadlines, plus process tests.
  # Its outer budget must cover their sum; the individual protocol deadlines remain unchanged.
  set_tests_properties(mux.terminal_boundary.clipboard PROPERTIES TIMEOUT 90)
  frame_add_python_mux_test(terminal_boundary.focus
                            tests.mux.test_terminal_boundary.FocusReportMuxTest)
  frame_add_python_mux_test(terminal_boundary.graphics
                            tests.mux.test_terminal_boundary.GraphicsMuxTest)
  frame_add_python_mux_test(terminal_boundary.terminal
                            tests.mux.test_terminal_boundary.TerminalBoundaryMuxTest)
  frame_add_python_mux_test(outer_terminal tests.mux.test_outer_terminal)
  # Rate-limit and fairness tests wait for real 5-second notification refills.
  set_tests_properties(mux.outer_terminal PROPERTIES TIMEOUT 90)
  frame_add_python_mux_test(agent tests.mux.test_agent)
  frame_add_python_mux_test(cli tests.mux.test_cli)
  frame_add_python_mux_test(process_boundary tests.mux.test_process_boundary)

  add_custom_target(
    frame_fast_test_binaries
    DEPENDS
      frame
      frame_unit_tests
      frame_terminal_boundary_tests
      frame_component_integration_tests
      frame_dependency_integrity_tests
      frame_benchmark_probe
      frame_test_server
      frame_test_cli
      frame_test_pty_peer
  )

  add_test(
    NAME stress.mux_state_machine
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "FRAME_TEST_SERVER=$<TARGET_FILE:frame_test_server>"
      "FRAME_TEST_CLI=$<TARGET_FILE:frame_test_cli>"
      "FRAME_TEST_PTY_PEER=$<TARGET_FILE:frame_test_pty_peer>"
      "${Python3_EXECUTABLE}" -m unittest -v tests.stress.test_mux_state_machine
  )
  set_tests_properties(
    stress.mux_state_machine
    PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      TIMEOUT 180
      LABELS "extended;integration;mux;stress"
      RUN_SERIAL TRUE
  )

  add_test(
    NAME stress.mux_backpressure
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "FRAME_TEST_SERVER=$<TARGET_FILE:frame_test_server>"
      "FRAME_TEST_CLI=$<TARGET_FILE:frame_test_cli>"
      "FRAME_TEST_PTY_PEER=$<TARGET_FILE:frame_test_pty_peer>"
      "${Python3_EXECUTABLE}" -m unittest -v tests.stress.test_backpressure
  )
  set_tests_properties(
    stress.mux_backpressure
    PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      TIMEOUT 60
      LABELS "extended;integration;mux;stress;backpressure"
      RUN_SERIAL TRUE
  )

  add_test(
    NAME stress.mux_trace_replay
    COMMAND
      "${CMAKE_COMMAND}" -E env
      "FRAME_TEST_SERVER=$<TARGET_FILE:frame_test_server>"
      "FRAME_TEST_CLI=$<TARGET_FILE:frame_test_cli>"
      "FRAME_TEST_PTY_PEER=$<TARGET_FILE:frame_test_pty_peer>"
      "${Python3_EXECUTABLE}" -m unittest -v tests.stress.test_mux_trace_replay
  )
  set_tests_properties(
    stress.mux_trace_replay
    PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      TIMEOUT 180
      LABELS "extended;integration;mux;stress;trace-replay"
      RUN_SERIAL TRUE
  )

  add_executable(frame_steady_state_allocation_audit tests/steady_state_allocation_audit.cpp)
  frame_internal_target(frame_steady_state_allocation_audit)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # The audit replaces every sized delete form so malloc-backed global new remains ASan-correct.
    target_compile_options(frame_steady_state_allocation_audit PRIVATE -fsized-deallocation)
  endif()
  target_link_libraries(
    frame_steady_state_allocation_audit
    PRIVATE frame_render frame_runtime frame_terminal
  )
  add_test(
    NAME steady_state_allocation_audit
    COMMAND frame_steady_state_allocation_audit
  )
  set_tests_properties(
    steady_state_allocation_audit
    PROPERTIES
      # Fixed-work correctness audit, not a wall-clock performance gate. Leave headroom for
      # sanitizer instrumentation on shared runners; allocation and work budgets stay exact.
      TIMEOUT 120
      LABELS "allocation;extended;performance"
      RUN_SERIAL TRUE
  )
endif()

if(FRAME_BUILD_FUZZERS)
  function(frame_add_fuzzer target source)
    if(APPLE)
      # Xcode Clang does not ship libclang_rt.fuzzer_osx. Keep the same entry points and sanitizer
      # instrumentation, but link a corpus replay driver so Darwin can verify every checked-in seed.
      add_executable(${target} ${source} fuzz/corpus_driver.cpp)
      set(fuzzer_sanitizers -fsanitize=address,undefined)
    else()
      add_executable(${target} ${source})
      set(fuzzer_sanitizers -fsanitize=fuzzer,address,undefined)
    endif()
    frame_internal_target(${target})
    target_compile_options(${target} PRIVATE ${fuzzer_sanitizers} -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE ${fuzzer_sanitizers} -fno-omit-frame-pointer)
    target_link_libraries(${target} PRIVATE ${ARGN})
  endfunction()

  frame_add_fuzzer(
    frame_attachment_decoder_fuzz fuzz/attachment_decoder_fuzz.cpp frame_protocol
  )
  frame_add_fuzzer(
    frame_host_input_parser_fuzz fuzz/host_input_parser_fuzz.cpp frame_client
  )
  frame_add_fuzzer(frame_api_json_fuzz fuzz/api_json_fuzz.cpp frame_api)
  frame_add_fuzzer(
    frame_extension_framing_fuzz fuzz/extension_framing_fuzz.cpp frame_base frame_platform
  )
  # Compile the production decoder with libFuzzer coverage too, not just the harness. Linking the
  # ordinary static library gives sanitizer checks but no decoder-guided mutation feedback.
  target_sources(frame_extension_framing_fuzz PRIVATE src/extension/protocol.cpp)
endif()

if(FRAME_BUILD_BENCHMARKS)
  find_package(benchmark CONFIG REQUIRED)

  add_executable(frame_benchmarks benchmarks/frame_benchmark.cpp)
  target_link_libraries(
    frame_benchmarks
    PRIVATE frame::lib frame_warnings benchmark::benchmark
  )

  add_executable(frame_memory_census benchmarks/terminal_memory.cpp)
  frame_internal_target(frame_memory_census)
  target_link_libraries(frame_memory_census PRIVATE frame_render frame_terminal)
endif()
