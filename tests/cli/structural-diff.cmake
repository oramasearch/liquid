# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_STRUCTURAL_DIFF_FIXTURE OR
   NOT DEFINED LIQUID_STRUCTURAL_DIFF_ROOT)
  message(FATAL_ERROR "structural diff test arguments are required")
endif()

file(REMOVE_RECURSE "${LIQUID_STRUCTURAL_DIFF_ROOT}")
execute_process(
  COMMAND "${LIQUID_STRUCTURAL_DIFF_FIXTURE}" "${LIQUID_STRUCTURAL_DIFF_ROOT}"
  RESULT_VARIABLE fixture_result
  ERROR_VARIABLE fixture_error)
if(NOT fixture_result EQUAL 0)
  message(FATAL_ERROR "structural diff fixture failed: ${fixture_error}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" diff before after --structural
  WORKING_DIRECTORY "${LIQUID_STRUCTURAL_DIFF_ROOT}"
  RESULT_VARIABLE diff_result
  OUTPUT_VARIABLE diff_output
  ERROR_VARIABLE diff_error)
if(NOT diff_result EQUAL 0)
  message(FATAL_ERROR "liquid structural diff failed: ${diff_error}")
endif()
if(NOT diff_output MATCHES "moved function alpha:")
  message(FATAL_ERROR "function move is absent: ${diff_output}")
endif()
if(NOT diff_output MATCHES "reformat only format.c")
  message(FATAL_ERROR "format-only label is absent: ${diff_output}")
endif()
string(FIND "${diff_output}" "text (no grammar for .abc) notes.abc" fallback_position)
if(fallback_position EQUAL -1)
  message(FATAL_ERROR "text fallback label is absent: ${diff_output}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" diff before after functions.c --structural --json
  WORKING_DIRECTORY "${LIQUID_STRUCTURAL_DIFF_ROOT}"
  RESULT_VARIABLE json_result
  OUTPUT_VARIABLE json_output
  ERROR_VARIABLE json_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT json_result EQUAL 0)
  message(FATAL_ERROR "liquid structural diff JSON failed: ${json_error}")
endif()
string(JSON json_entries LENGTH "${json_output}")
string(JSON json_path GET "${json_output}" 0 path)
string(JSON json_tier GET "${json_output}" 0 diff_tier)
string(JSON json_language GET "${json_output}" 0 structural language)
string(JSON json_edit GET "${json_output}" 0 structural edits 0 kind)
if(NOT json_entries EQUAL 1 OR NOT json_path STREQUAL "functions.c" OR
   NOT json_tier STREQUAL "structural" OR NOT json_language STREQUAL "c" OR
   NOT json_edit STREQUAL "moved")
  message(FATAL_ERROR "liquid structural diff JSON changed: ${json_output}")
endif()
