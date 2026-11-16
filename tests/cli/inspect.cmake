# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_INSPECT_FIXTURE)
  message(FATAL_ERROR "LIQUID_CLI and LIQUID_INSPECT_FIXTURE are required")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" inspect "${LIQUID_INSPECT_FIXTURE}" --json
  RESULT_VARIABLE first_status
  OUTPUT_VARIABLE first_output
  ERROR_VARIABLE first_error)
if(NOT first_status EQUAL 0)
  message(FATAL_ERROR "first inspect failed: ${first_error}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" inspect "${LIQUID_INSPECT_FIXTURE}" --json
  RESULT_VARIABLE second_status
  OUTPUT_VARIABLE second_output
  ERROR_VARIABLE second_error)
if(NOT second_status EQUAL 0)
  message(FATAL_ERROR "second inspect failed: ${second_error}")
endif()

if(NOT first_output STREQUAL second_output)
  message(FATAL_ERROR "inspect JSON is not stable")
endif()

string(FIND "${first_output}" "\"codec\":\"zip\"" codec_offset)
if(codec_offset EQUAL -1)
  message(FATAL_ERROR "inspect JSON did not identify the ZIP codec")
endif()
