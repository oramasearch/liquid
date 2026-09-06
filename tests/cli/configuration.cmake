if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_CONFIG_ROOT)
  message(FATAL_ERROR "configuration CLI test arguments are required")
endif()

file(REMOVE_RECURSE "${LIQUID_CONFIG_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_CONFIG_ROOT}")
set(configuration "${LIQUID_CONFIG_ROOT}/liquid.liquid")
file(WRITE "${configuration}"
  "package demo{versions:[1.2.3]}\n"
  "build app{outputs:[path \"out/app\"] command:[\"cc\",\"main.c\"]}\n")

execute_process(
  COMMAND "${LIQUID_CLI}" config check "${LIQUID_CONFIG_ROOT}"
  RESULT_VARIABLE check_result
  ERROR_VARIABLE check_error)
if(NOT check_result EQUAL 0)
  message(FATAL_ERROR "configuration check failed: ${check_error}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" config lower "${configuration}" --json
  RESULT_VARIABLE lower_result
  OUTPUT_VARIABLE lower_output
  ERROR_VARIABLE lower_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT lower_result EQUAL 0)
  message(FATAL_ERROR "configuration lower failed: ${lower_error}")
endif()
string(JSON package_name GET "${lower_output}" packages 0 name)
string(JSON target_name GET "${lower_output}" targets 0 name)
if(NOT package_name STREQUAL "demo" OR NOT target_name STREQUAL "app")
  message(FATAL_ERROR "configuration lower JSON changed: ${lower_output}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" config lower "${configuration}" --trace
  RESULT_VARIABLE trace_result
  OUTPUT_VARIABLE trace_output
  ERROR_VARIABLE trace_error)
if(NOT trace_result EQUAL 0)
  message(FATAL_ERROR "configuration trace failed: ${trace_error}")
endif()
if(NOT trace_output MATCHES "package demo origin=\\{\"kind\":\"source\"")
  message(FATAL_ERROR "configuration package trace changed: ${trace_output}")
endif()
if(NOT trace_output MATCHES "target app origin=\\{\"kind\":\"source\"")
  message(FATAL_ERROR "configuration target trace changed: ${trace_output}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" config format "${configuration}"
  RESULT_VARIABLE format_result
  ERROR_VARIABLE format_error)
if(NOT format_result EQUAL 0)
  message(FATAL_ERROR "configuration format failed: ${format_error}")
endif()
file(READ "${configuration}" first_format)
execute_process(
  COMMAND "${LIQUID_CLI}" config format "${configuration}"
  RESULT_VARIABLE second_result
  ERROR_VARIABLE second_error)
if(NOT second_result EQUAL 0)
  message(FATAL_ERROR "second configuration format failed: ${second_error}")
endif()
file(READ "${configuration}" second_format)
if(NOT first_format STREQUAL second_format)
  message(FATAL_ERROR "configuration format is not idempotent")
endif()
