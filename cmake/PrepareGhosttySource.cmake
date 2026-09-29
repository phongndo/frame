# The same preparation path serves immutable Nix inputs and clean Git submodules.
include("${CMAKE_CURRENT_LIST_DIR}/ValidateGhosttyPin.cmake")
if(NOT DEFINED GHOSTTY_PATCHED_SOURCE_DIR OR GHOSTTY_PATCH_FILES STREQUAL "")
  message(FATAL_ERROR "Ghostty source preparation requires patches and a destination")
endif()
file(REAL_PATH "${GHOSTTY_SOURCE_DIR}" original_source)
file(MAKE_DIRECTORY "${GHOSTTY_PATCHED_SOURCE_DIR}")
file(REAL_PATH "${GHOSTTY_PATCHED_SOURCE_DIR}" patched_source)
if(original_source STREQUAL patched_source)
  message(FATAL_ERROR "Ghostty patches must not modify the original source")
endif()

# Like the archive, this is a derived build cache, keyed by the complete pin manifest.
file(SHA256 "${GHOSTTY_PIN_FILE}" identity)
set(stamp "${patched_source}/.frame-patched-source")
if(EXISTS "${stamp}" AND EXISTS "${patched_source}/build.zig")
  file(READ "${stamp}" prepared_identity)
  if(prepared_identity STREQUAL identity)
    return()
  endif()
endif()

find_program(GHOSTTY_PATCH_EXECUTABLE patch REQUIRED)
set(staging "${patched_source}.tmp")
file(REMOVE_RECURSE "${staging}")
file(MAKE_DIRECTORY "${staging}")
file(COPY "${original_source}/" DESTINATION "${staging}"
  DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE
  PATTERN ".git" EXCLUDE
  PATTERN ".zig-cache" EXCLUDE
  PATTERN "zig-out" EXCLUDE)
foreach(patch_file IN LISTS GHOSTTY_PATCH_FILES)
  execute_process(
    COMMAND "${GHOSTTY_PATCH_EXECUTABLE}" --batch --forward --fuzz=0 -p1 -i "${patch_file}"
    WORKING_DIRECTORY "${staging}"
    RESULT_VARIABLE patch_result
    OUTPUT_VARIABLE patch_output
    ERROR_VARIABLE patch_error)
  if(NOT patch_result EQUAL 0)
    file(REMOVE_RECURSE "${staging}")
    message(FATAL_ERROR "Ghostty patch failed: ${patch_file}\n${patch_output}\n${patch_error}")
  endif()
endforeach()
file(WRITE "${staging}/.frame-patched-source" "${identity}")
file(REMOVE_RECURSE "${patched_source}")
file(RENAME "${staging}" "${patched_source}")
