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

file(MAKE_DIRECTORY "${LIQUID_CONFIG_ROOT}/records")
set(record "${LIQUID_CONFIG_ROOT}/records/demo.cbor")
execute_process(
  COMMAND "${LIQUID_CLI}" config lower "${configuration}" --output-record "${record}"
  RESULT_VARIABLE record_result
  OUTPUT_VARIABLE record_output
  ERROR_VARIABLE record_error)
if(NOT record_result EQUAL 0 OR NOT record_output STREQUAL "")
  message(FATAL_ERROR "canonical record output failed: ${record_output}${record_error}")
endif()
execute_process(
  COMMAND "${LIQUID_CLI}" config lower "${configuration}" --output-record -
  RESULT_VARIABLE stdout_result
  OUTPUT_FILE "${LIQUID_CONFIG_ROOT}/stdout.cbor"
  ERROR_VARIABLE stdout_error)
file(SHA256 "${record}" file_digest)
file(SHA256 "${LIQUID_CONFIG_ROOT}/stdout.cbor" stdout_digest)
if(NOT stdout_result EQUAL 0 OR NOT file_digest STREQUAL stdout_digest)
  message(FATAL_ERROR "binary stdout differs from file output: ${stdout_error}")
endif()
execute_process(
  COMMAND "${LIQUID_CLI}" resolve demo --universe "${LIQUID_CONFIG_ROOT}/records" --json
  RESULT_VARIABLE resolve_result
  OUTPUT_VARIABLE resolve_output
  ERROR_VARIABLE resolve_error)
if(NOT resolve_result EQUAL 0)
  message(FATAL_ERROR "lowered record does not resolve: ${resolve_error}")
endif()
string(JSON resolved_version GET "${resolve_output}" nodes 0 package_version)
if(NOT resolved_version STREQUAL "1.2.3")
  message(FATAL_ERROR "lowered record lost its version: ${resolve_output}")
endif()

function(expect_configuration_failure expected_status expected_text)
  execute_process(
    COMMAND "${LIQUID_CLI}" ${ARGN}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  string(FIND "${error}" "${expected_text}" found)
  if(NOT status EQUAL expected_status OR found EQUAL -1)
    message(FATAL_ERROR "expected exit ${expected_status} and '${expected_text}': ${status}: ${output}${error}")
  endif()
endfunction()

expect_configuration_failure(2 "cannot be combined"
  config lower "${configuration}" --output-record - --json)
expect_configuration_failure(2 "cannot be combined"
  config lower "${configuration}" --output-record - --trace)
expect_configuration_failure(2 "requires --output-record"
  config lower "${configuration}" --package demo)
expect_configuration_failure(1 "no package named 'missing'"
  config lower "${configuration}" --output-record "${record}" --package missing)
expect_configuration_failure(1 "cannot write package record"
  config lower "${configuration}" --output-record "${LIQUID_CONFIG_ROOT}/absent/demo.cbor")

set(multiple "${LIQUID_CONFIG_ROOT}/multiple.liquid")
file(WRITE "${multiple}" "package first { versions: [1.0] }\npackage second { versions: [2.0] }\n")
expect_configuration_failure(1 "requires exactly one package"
  config lower "${multiple}" --output-record "${record}")
execute_process(
  COMMAND "${LIQUID_CLI}" config lower "${multiple}" --package second --output-record "${record}"
  RESULT_VARIABLE selected_result
  ERROR_VARIABLE selected_error)
if(NOT selected_result EQUAL 0)
  message(FATAL_ERROR "package selection failed: ${selected_error}")
endif()
execute_process(
  COMMAND "${LIQUID_CLI}" resolve second --universe "${LIQUID_CONFIG_ROOT}/records"
  RESULT_VARIABLE selected_resolve_result
  ERROR_VARIABLE selected_resolve_error)
if(NOT selected_resolve_result EQUAL 0)
  message(FATAL_ERROR "selected package record does not resolve: ${selected_resolve_error}")
endif()

foreach(kind IN ITEMS empty unsupported)
  set(universe "${LIQUID_CONFIG_ROOT}/${kind}")
  file(MAKE_DIRECTORY "${universe}")
  if(kind STREQUAL "unsupported")
    file(WRITE "${universe}/example.txt" "package example { versions: [1.0] }\n")
  endif()
  foreach(option IN ITEMS --explain --json)
    expect_configuration_failure(1
      "No candidates were loaded from ${universe}; supply .liquid declarations or canonical .cbor records"
      resolve example --universe "${universe}" "${option}")
  endforeach()
endforeach()

set(universe "${LIQUID_CONFIG_ROOT}/contradictory")
file(MAKE_DIRECTORY "${universe}")
file(WRITE "${universe}/app.liquid"
  "package app { versions: [1.0] depends_on library { version: 2.0 } }\n")
file(WRITE "${universe}/library.liquid" "package library { versions: [1.0] }\n")
expect_configuration_failure(1
  "Resolution is unsatisfiable.\n- root app: root requires app\n- dependency app: app requires library@2.0\nNo configuration satisfies all 2 constraints."
  resolve app --universe "${universe}" --explain)
