# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI_SOURCE)
  message(FATAL_ERROR "LIQUID_CLI_SOURCE is required")
endif()

file(GLOB command_sources
  "${LIQUID_CLI_SOURCE}/src/main.cpp"
  "${LIQUID_CLI_SOURCE}/src/cli/*.cpp")
foreach(source IN LISTS command_sources)
  file(STRINGS "${source}" includes REGEX "^[ \\t]*#include \\\"[^\\\"]+\\\"")
  foreach(include IN LISTS includes)
    string(REGEX REPLACE "^[ \\t]*#include \\\"([^\\\"]+)\\\"" "\\1" header "${include}")
    if(NOT header MATCHES "^(liquid/|cli/)")
      message(FATAL_ERROR "${source} includes non-public header ${header}")
    endif()
  endforeach()
endforeach()
