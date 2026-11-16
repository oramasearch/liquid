# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_CLI_ROOT)
  message(FATAL_ERROR "LIQUID_CLI and LIQUID_CLI_ROOT are required")
endif()

function(run directory)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "LIQUID_HOME=${LIQUID_CLI_ROOT}/identity"
      "${LIQUID_CLI}" ${ARGN}
    WORKING_DIRECTORY "${directory}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "liquid ${ARGN} exited ${result}:\n${output}${error}")
  endif()
  message(STATUS "${output}")
  set(last_output "${output}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${LIQUID_CLI_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_CLI_ROOT}/source/sub" "${LIQUID_CLI_ROOT}/other")
file(WRITE "${LIQUID_CLI_ROOT}/source/sub/hello.txt" "hello split\n")
file(WRITE "${LIQUID_CLI_ROOT}/other/asset.txt" "nested bytes\n")
run("${LIQUID_CLI_ROOT}/source" init .)
run("${LIQUID_CLI_ROOT}/source" cut "first version")
run("${LIQUID_CLI_ROOT}/source" split sub ../split-out)
file(RENAME "${LIQUID_CLI_ROOT}/source" "${LIQUID_CLI_ROOT}/source-moved-away")
run("${LIQUID_CLI_ROOT}/split-out" materialise ../export)
file(READ "${LIQUID_CLI_ROOT}/export/hello.txt" bytes)
if(NOT bytes STREQUAL "hello split\n")
  message(FATAL_ERROR "split materialised different bytes")
endif()
run("${LIQUID_CLI_ROOT}/split-out" log)
if(NOT last_output MATCHES "first version")
  message(FATAL_ERROR "split lost prior history: ${last_output}")
endif()

run("${LIQUID_CLI_ROOT}/other" init .)
run("${LIQUID_CLI_ROOT}/other" cut "asset history")
run("${LIQUID_CLI_ROOT}/source-moved-away" nest ../other vendor/assets)
run("${LIQUID_CLI_ROOT}/source-moved-away" cut "nest")
file(REMOVE_RECURSE "${LIQUID_CLI_ROOT}/other")
run("${LIQUID_CLI_ROOT}/source-moved-away" split vendor/assets ../nested-out)
file(REMOVE_RECURSE "${LIQUID_CLI_ROOT}/source-moved-away")
run("${LIQUID_CLI_ROOT}/nested-out" materialise ../nested-export)
file(READ "${LIQUID_CLI_ROOT}/nested-export/asset.txt" bytes)
if(NOT bytes STREQUAL "nested bytes\n")
  message(FATAL_ERROR "nested split materialised different bytes")
endif()
run("${LIQUID_CLI_ROOT}/nested-out" log)
if(NOT last_output MATCHES "asset history")
  message(FATAL_ERROR "nested split lost prior history: ${last_output}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "LIQUID_HOME=${LIQUID_CLI_ROOT}/identity"
    "${LIQUID_CLI}" split missing ../failed-out
  WORKING_DIRECTORY "${LIQUID_CLI_ROOT}/split-out"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${LIQUID_CLI_ROOT}/failed-out")
  message(FATAL_ERROR "failed split left a destination: ${output}${error}")
endif()
message(STATUS "Both split reproductions preserve bytes and history after source removal")
