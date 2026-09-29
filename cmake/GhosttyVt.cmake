include_guard(GLOBAL)

# Build the pinned libghostty-vt without writing generated files into the Git submodule. The output
# and both Zig caches are scoped to the active CMake binary tree and Ghostty commit.
function(frame_add_pinned_ghostty)
  set(pin_file "${CMAKE_SOURCE_DIR}/third_party/ghostty-metadata/PIN.json")
  set(
    FRAME_GHOSTTY_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/ghostty"
    CACHE PATH "Pinned Ghostty source tree"
  )
  set(
    FRAME_GHOSTTY_NIX_SOURCE_REV ""
    CACHE STRING "Locked revision for an immutable Ghostty Nix source"
  )
  set(
    FRAME_GHOSTTY_ZIG_SYSTEM_DIR ""
    CACHE PATH "Offline Zig dependency directory"
  )
  set(
    FRAME_GHOSTTY_ZIG_LIBC ""
    CACHE FILEPATH "Zig libc installation description for sandboxed native builds"
  )
  set(
    FRAME_GHOSTTY_ZIG_TARGET ""
    CACHE STRING "Explicit Zig target for sandboxed packaged builds"
  )
  set(source_dir "${FRAME_GHOSTTY_SOURCE_DIR}")

  if(NOT EXISTS "${pin_file}")
    message(FATAL_ERROR "missing Ghostty pin metadata: ${pin_file}")
  endif()

  file(READ "${pin_file}" pin_json)
  string(JSON pinned_commit GET "${pin_json}" commit)
  string(JSON pinned_zig GET "${pin_json}" zig_version)
  string(JSON expected_version GET "${pin_json}" expected_version)
  string(JSON emit_xcframework GET "${pin_json}" build_options emit_xcframework)
  string(JSON expected_simd GET "${pin_json}" expected_build_features simd)
  string(JSON expected_kitty_graphics GET "${pin_json}" expected_build_features kitty_graphics)
  string(JSON expected_tmux_control GET "${pin_json}" expected_build_features tmux_control_mode)

  set(GHOSTTY_PIN_FILE "${pin_file}")
  set(GHOSTTY_SOURCE_DIR "${source_dir}")
  set(GHOSTTY_NIX_SOURCE_REV "${FRAME_GHOSTTY_NIX_SOURCE_REV}")
  set(
    pin_validation_args
    "-DGHOSTTY_PIN_FILE=${pin_file}"
    "-DGHOSTTY_SOURCE_DIR=${source_dir}"
  )
  if(FRAME_GHOSTTY_NIX_SOURCE_REV)
    list(
      APPEND pin_validation_args
      "-DGHOSTTY_NIX_SOURCE_REV=${FRAME_GHOSTTY_NIX_SOURCE_REV}"
    )
  else()
    find_package(Git REQUIRED)
    set(GHOSTTY_GIT_EXECUTABLE "${GIT_EXECUTABLE}")
    list(APPEND pin_validation_args "-DGHOSTTY_GIT_EXECUTABLE=${GIT_EXECUTABLE}")
  endif()
  set(pin_validator "${CMAKE_SOURCE_DIR}/cmake/ValidateGhosttyPin.cmake")
  include("${pin_validator}")

  find_program(ZIG_EXECUTABLE zig REQUIRED)
  set(zig_system_args)
  if(FRAME_GHOSTTY_ZIG_SYSTEM_DIR)
    list(APPEND zig_system_args --system "${FRAME_GHOSTTY_ZIG_SYSTEM_DIR}")
  endif()
  set(zig_libc_args)
  if(FRAME_GHOSTTY_ZIG_LIBC)
    list(APPEND zig_libc_args --libc "${FRAME_GHOSTTY_ZIG_LIBC}")
  endif()
  set(zig_target_args)
  if(FRAME_GHOSTTY_ZIG_TARGET)
    list(APPEND zig_target_args "-Dtarget=${FRAME_GHOSTTY_ZIG_TARGET}")
  endif()
  execute_process(
    COMMAND "${ZIG_EXECUTABLE}" version
    RESULT_VARIABLE zig_result
    OUTPUT_VARIABLE actual_zig
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT zig_result EQUAL 0 OR NOT actual_zig STREQUAL pinned_zig)
    message(FATAL_ERROR "Ghostty requires Zig ${pinned_zig}; found ${actual_zig}")
  endif()

  if(NOT CMAKE_BUILD_TYPE)
    message(FATAL_ERROR "a single-config CMAKE_BUILD_TYPE is required for the pinned Ghostty build")
  endif()
  string(JSON optimize ERROR_VARIABLE optimize_error
    GET "${pin_json}" optimization_by_cmake_build_type "${CMAKE_BUILD_TYPE}"
  )
  if(optimize_error)
    message(FATAL_ERROR
      "PIN.json has no Ghostty optimization mapping for CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    )
  endif()

  if(emit_xcframework)
    set(xcframework_flag true)
  else()
    set(xcframework_flag false)
  endif()
  if(expected_simd)
    set(simd_flag true)
    set(simd_definition 1)
  else()
    set(simd_flag false)
    set(simd_definition 0)
  endif()
  if(expected_kitty_graphics)
    set(kitty_graphics_definition 1)
  else()
    set(kitty_graphics_definition 0)
  endif()
  if(expected_tmux_control)
    set(tmux_control_definition 1)
  else()
    set(tmux_control_definition 0)
  endif()
  if(optimize STREQUAL "Debug")
    set(optimize_definition 0)
  elseif(optimize STREQUAL "ReleaseSafe")
    set(optimize_definition 1)
  elseif(optimize STREQUAL "ReleaseSmall")
    set(optimize_definition 2)
  elseif(optimize STREQUAL "ReleaseFast")
    set(optimize_definition 3)
  else()
    message(FATAL_ERROR "unsupported Ghostty optimization mode in PIN.json: ${optimize}")
  endif()

  file(SHA256 "${pin_file}" pin_identity)
  set(root "${CMAKE_BINARY_DIR}/_deps/ghostty/${pinned_commit}-${pin_identity}")
  set(patch_preparer "${CMAKE_SOURCE_DIR}/cmake/PrepareGhosttySource.cmake")
  set(source_preparation_command "${CMAKE_COMMAND}" ${pin_validation_args} -P "${pin_validator}")
  if(GHOSTTY_PATCH_FILES)
    set(source_dir "${root}/source")
    set(source_preparation_command
      "${CMAKE_COMMAND}" ${pin_validation_args}
      "-DGHOSTTY_PATCHED_SOURCE_DIR=${source_dir}" -P "${patch_preparer}")
    execute_process(COMMAND ${source_preparation_command} RESULT_VARIABLE preparation_result)
    if(NOT preparation_result EQUAL 0)
      message(FATAL_ERROR "failed to prepare patched Ghostty source")
    endif()
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${pin_file}" ${GHOSTTY_PATCH_FILES} "${patch_preparer}")
  set(prefix "${root}/${CMAKE_BUILD_TYPE}")
  set(local_cache "${root}/zig-cache/local-${CMAKE_BUILD_TYPE}")
  set(global_cache "${root}/zig-cache/global")
  if(WIN32)
    set(static_library "${prefix}/lib/ghostty-vt-static.lib")
  else()
    set(static_library "${prefix}/lib/libghostty-vt.a")
  endif()
  set(include_dir "${prefix}/include")
  file(MAKE_DIRECTORY "${include_dir}")

  add_custom_target(
    frame_ghostty_vt_validate
    COMMAND "${CMAKE_COMMAND}" ${pin_validation_args} -P "${pin_validator}"
    COMMENT "Validating pinned Ghostty source"
    VERBATIM
  )
  add_custom_command(
    OUTPUT "${static_library}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${local_cache}" "${global_cache}"
    COMMAND ${source_preparation_command}
    COMMAND
      "${ZIG_EXECUTABLE}" build
      ${zig_system_args}
      ${zig_libc_args}
      --prefix "${prefix}"
      --cache-dir "${local_cache}"
      --global-cache-dir "${global_cache}"
      -Demit-lib-vt=true
      -Demit-xcframework=${xcframework_flag}
      -Dsimd=${simd_flag}
      ${zig_target_args}
      -Doptimize=${optimize}
    WORKING_DIRECTORY "${source_dir}"
    DEPENDS
      "${pin_file}"
      "${pin_validator}"
      "${patch_preparer}"
      ${GHOSTTY_PATCH_FILES}
      "${source_dir}/build.zig"
      "${source_dir}/build.zig.zon"
    COMMENT "Building pinned libghostty-vt ${pinned_commit} (${optimize})"
    VERBATIM
    USES_TERMINAL
  )
  add_custom_target(frame_ghostty_vt_build DEPENDS "${static_library}")
  if(FRAME_VALIDATE_GHOSTTY_EVERY_BUILD)
    add_dependencies(frame_ghostty_vt_build frame_ghostty_vt_validate)
  endif()
  # Preserve the target name used by analysis scripts and existing embedders.
  add_custom_target(zig_build_lib_vt)
  add_dependencies(zig_build_lib_vt frame_ghostty_vt_build)

  add_library(ghostty-vt-static STATIC IMPORTED GLOBAL)
  set_target_properties(
    ghostty-vt-static
    PROPERTIES
      IMPORTED_LOCATION "${static_library}"
      IMPORTED_LOCATION_RELEASE "${static_library}"
      INTERFACE_INCLUDE_DIRECTORIES "${include_dir}"
      INTERFACE_COMPILE_DEFINITIONS
        "GHOSTTY_STATIC;FRAME_GHOSTTY_EXPECT_VERSION=\"${expected_version}\";FRAME_GHOSTTY_EXPECT_SIMD=${simd_definition};FRAME_GHOSTTY_EXPECT_KITTY_GRAPHICS=${kitty_graphics_definition};FRAME_GHOSTTY_EXPECT_TMUX_CONTROL_MODE=${tmux_control_definition};FRAME_GHOSTTY_EXPECT_OPTIMIZE=${optimize_definition}"
  )
  if(WIN32)
    set_property(
      TARGET ghostty-vt-static PROPERTY INTERFACE_LINK_LIBRARIES "ntdll;kernel32"
    )
  endif()
  add_dependencies(ghostty-vt-static frame_ghostty_vt_build)

  message(STATUS "Pinned Ghostty: ${pinned_commit}")
  message(STATUS "Ghostty output: ${prefix}")
endfunction()
