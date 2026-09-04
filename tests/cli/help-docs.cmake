if(NOT DEFINED LIQUID_CLI)
  message(FATAL_ERROR "LIQUID_CLI is required")
endif()
if(NOT DEFINED LIQUID_CLI_DOCS)
  message(FATAL_ERROR "LIQUID_CLI_DOCS is required")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" --help
  RESULT_VARIABLE help_result
  OUTPUT_VARIABLE help_output
  ERROR_VARIABLE help_error)
if(NOT help_result EQUAL 0)
  message(FATAL_ERROR "liquid --help failed: ${help_error}")
endif()

file(READ "${LIQUID_CLI_DOCS}" documentation)
string(REGEX MATCHALL "\\n  [a-z][a-z_]* +" help_rows "\\n${help_output}")
foreach(row IN LISTS help_rows)
  string(REGEX REPLACE "^[\\n ]*([^ ]+).*" "\\1" command "${row}")
  string(FIND "${documentation}" "| `${command}` |" documentation_position)
  if(documentation_position EQUAL -1)
    message(FATAL_ERROR "documentation is missing ${command}")
  endif()
endforeach()

string(REGEX MATCHALL "\\| `[a-z][a-z_]*` \\|" documentation_rows "${documentation}")
foreach(row IN LISTS documentation_rows)
  string(REPLACE "|" ";" columns "${row}")
  list(GET columns 1 command)
  string(STRIP "${command}" command)
  string(REPLACE "`" "" command "${command}")
  string(FIND "${help_output}" "  ${command} " help_position)
  if(help_position EQUAL -1)
    message(FATAL_ERROR "--help is missing documented command ${command}")
  endif()
endforeach()
