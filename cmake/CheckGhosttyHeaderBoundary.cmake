if(NOT DEFINED FRAME_SOURCE_DIR)
  message(FATAL_ERROR "FRAME_SOURCE_DIR is required")
endif()

file(
  GLOB_RECURSE frame_sources
  LIST_DIRECTORIES false
  "${FRAME_SOURCE_DIR}/apps/*.cpp"
  "${FRAME_SOURCE_DIR}/include/*.hpp"
  "${FRAME_SOURCE_DIR}/src/*.cpp"
  "${FRAME_SOURCE_DIR}/src/*.hpp"
  "${FRAME_SOURCE_DIR}/tests/*.cpp"
  "${FRAME_SOURCE_DIR}/tests/*.hpp"
)

set(violations)
foreach(source IN LISTS frame_sources)
  file(READ "${source}" contents)
  if(contents MATCHES "#[ \t]*include[ \t]*[<\"]ghostty/")
    file(RELATIVE_PATH relative "${FRAME_SOURCE_DIR}" "${source}")
    if(NOT relative MATCHES "^src/terminal/")
      list(APPEND violations "${relative}")
    endif()
  endif()
endforeach()

if(violations)
  list(JOIN violations ", " encoded)
  message(FATAL_ERROR "Ghostty headers escaped the terminal boundary: ${encoded}")
endif()
