# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_BLAME_ROOT)
  message(FATAL_ERROR "LIQUID_CLI and LIQUID_BLAME_ROOT are required")
endif()

file(REMOVE_RECURSE "${LIQUID_BLAME_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_BLAME_ROOT}")
file(WRITE "${LIQUID_BLAME_ROOT}/notes.txt" "first\nsecond\n")

execute_process(
  COMMAND "${LIQUID_CLI}" init .
  WORKING_DIRECTORY "${LIQUID_BLAME_ROOT}"
  RESULT_VARIABLE init_result
  ERROR_VARIABLE init_error)
if(NOT init_result EQUAL 0)
  message(FATAL_ERROR "blame fixture init failed: ${init_error}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" cut initial
  WORKING_DIRECTORY "${LIQUID_BLAME_ROOT}"
  RESULT_VARIABLE cut_result
  ERROR_VARIABLE cut_error)
if(NOT cut_result EQUAL 0)
  message(FATAL_ERROR "blame fixture cut failed: ${cut_error}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" blame notes.txt --json
  WORKING_DIRECTORY "${LIQUID_BLAME_ROOT}"
  RESULT_VARIABLE json_result
  OUTPUT_VARIABLE json_output
  ERROR_VARIABLE json_error)
if(NOT json_result EQUAL 0)
  message(FATAL_ERROR "blame JSON failed: ${json_error}")
endif()
if(NOT json_output MATCHES
   "^\\{\"v\":1,\"path\":\"notes.txt\",\"change\":\"blake3:[0-9a-f]+\",\"lines\":\\[\\{\"line\":1")
  message(FATAL_ERROR "unexpected blame JSON: ${json_output}")
endif()
if(NOT json_output MATCHES "\"agent\":false,\"text\":\"second\"")
  message(FATAL_ERROR "blame JSON is missing its second line: ${json_output}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" blame notes.txt
  WORKING_DIRECTORY "${LIQUID_BLAME_ROOT}"
  RESULT_VARIABLE text_result
  OUTPUT_VARIABLE text_output
  ERROR_VARIABLE text_error)
if(NOT text_result EQUAL 0)
  message(FATAL_ERROR "blame text failed: ${text_error}")
endif()
if(NOT text_output MATCHES "1 change=blake3:[0-9a-f]+ session=[0-9a-f]+ actor=.+ agent=false")
  message(FATAL_ERROR "unexpected blame text: ${text_output}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" daemon --shutdown
  WORKING_DIRECTORY "${LIQUID_BLAME_ROOT}"
  OUTPUT_QUIET
  ERROR_QUIET)
