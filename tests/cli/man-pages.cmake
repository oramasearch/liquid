if(NOT DEFINED LIQUID_CLI)
  message(FATAL_ERROR "LIQUID_CLI is required")
endif()
if(NOT DEFINED LIQUID_MAN_OUTPUT)
  message(FATAL_ERROR "LIQUID_MAN_OUTPUT is required")
endif()

file(REMOVE_RECURSE "${LIQUID_MAN_OUTPUT}")
execute_process(
  COMMAND "${LIQUID_CLI}" docs --format man --out "${LIQUID_MAN_OUTPUT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "man-page generation failed: ${output}${error}")
endif()
if(NOT EXISTS "${LIQUID_MAN_OUTPUT}/liquid.1")
  message(FATAL_ERROR "man-page generation did not create liquid.1")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" --help
  OUTPUT_VARIABLE help_output
  RESULT_VARIABLE help_result)
if(NOT help_result EQUAL 0)
  message(FATAL_ERROR "liquid --help failed")
endif()
string(REPLACE "\n" ";" help_lines "${help_output}")
foreach(line IN LISTS help_lines)
  if(NOT line MATCHES "^  ([a-z][a-z0-9_-]*) +")
    continue()
  endif()
  set(command "${CMAKE_MATCH_1}")
  set(page "${LIQUID_MAN_OUTPUT}/liquid-${command}.1")
  if(NOT EXISTS "${page}")
    message(FATAL_ERROR "man-page generation is missing ${page}")
  endif()
  file(READ "${page}" contents)
  foreach(section IN ITEMS NAME SYNOPSIS DESCRIPTION OPTIONS AVAILABILITY EXAMPLES "EXIT STATUS")
    string(FIND "${contents}" ".SH ${section}" position)
    if(position EQUAL -1)
      message(FATAL_ERROR "${page} is missing ${section}")
    endif()
  endforeach()
endforeach()
