# SPDX-License-Identifier: MIT

file(REMOVE_RECURSE "${LIQUID_DELTA_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_DELTA_ROOT}")
file(WRITE "${LIQUID_DELTA_ROOT}/source.txt" "release artifact\nold payload\n")
file(WRITE "${LIQUID_DELTA_ROOT}/target.txt" "release artifact\nnew payload\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LIQUID_HOME=${LIQUID_DELTA_ROOT}/identity"
          "${LIQUID_CLI}" delta create source.txt target.txt
  WORKING_DIRECTORY "${LIQUID_DELTA_ROOT}"
  RESULT_VARIABLE create_result
  OUTPUT_VARIABLE create_output
  ERROR_VARIABLE create_error)
if(NOT create_result EQUAL 0)
  message(FATAL_ERROR "delta create failed: ${create_error}")
endif()
string(REGEX MATCH "delta=(blake3:[0-9a-f]+)" delta_match "${create_output}")
set(delta_id "${CMAKE_MATCH_1}")
string(REGEX MATCH "target=(blake3:[0-9a-f]+)" target_match "${create_output}")
set(target_id "${CMAKE_MATCH_1}")
if(delta_id STREQUAL "" OR target_id STREQUAL "")
  message(FATAL_ERROR "delta create omitted record identifiers: ${create_output}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LIQUID_HOME=${LIQUID_DELTA_ROOT}/identity"
          "${LIQUID_CLI}" delta apply "${delta_id}" source.txt applied.txt
  WORKING_DIRECTORY "${LIQUID_DELTA_ROOT}"
  RESULT_VARIABLE apply_result
  ERROR_VARIABLE apply_error)
if(NOT apply_result EQUAL 0)
  message(FATAL_ERROR "delta apply failed: ${apply_error}")
endif()
file(SHA256 "${LIQUID_DELTA_ROOT}/target.txt" target_hash)
file(SHA256 "${LIQUID_DELTA_ROOT}/applied.txt" applied_hash)
if(NOT target_hash STREQUAL applied_hash)
  message(FATAL_ERROR "delta apply output differs from target")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LIQUID_HOME=${LIQUID_DELTA_ROOT}/identity"
          "${LIQUID_CLI}" delta fetch "${target_id}" source.txt fetched.txt
  WORKING_DIRECTORY "${LIQUID_DELTA_ROOT}"
  RESULT_VARIABLE fetch_result
  OUTPUT_VARIABLE fetch_output
  ERROR_VARIABLE fetch_error)
if(NOT fetch_result EQUAL 0)
  message(FATAL_ERROR "delta fetch failed: ${fetch_error}")
endif()
if(NOT fetch_output MATCHES "method=delta")
  message(FATAL_ERROR "delta fetch did not use the catalog: ${fetch_output}")
endif()
file(SHA256 "${LIQUID_DELTA_ROOT}/fetched.txt" fetched_hash)
if(NOT target_hash STREQUAL fetched_hash)
  message(FATAL_ERROR "delta fetch output differs from target")
endif()
