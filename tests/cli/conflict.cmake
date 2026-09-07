if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_CONFLICT_FIXTURE OR
   NOT DEFINED LIQUID_CONFLICT_ROOT)
  message(FATAL_ERROR "LIQUID_CLI, LIQUID_CONFLICT_FIXTURE, and LIQUID_CONFLICT_ROOT are required")
endif()

file(REMOVE_RECURSE "${LIQUID_CONFLICT_ROOT}")
execute_process(
  COMMAND "${LIQUID_CONFLICT_FIXTURE}" "${LIQUID_CONFLICT_ROOT}"
  RESULT_VARIABLE fixture_result
  OUTPUT_VARIABLE fixture_output
  ERROR_VARIABLE fixture_error)
if(NOT fixture_result EQUAL 0)
  message(FATAL_ERROR "conflict fixture failed:\n${fixture_output}${fixture_error}")
endif()

function(run_conflict output error result)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      "LIQUID_HOME=${LIQUID_CONFLICT_ROOT}/identity" "${LIQUID_CLI}" ${ARGN}
    WORKING_DIRECTORY "${LIQUID_CONFLICT_ROOT}"
    RESULT_VARIABLE command_result
    OUTPUT_VARIABLE command_output
    ERROR_VARIABLE command_error)
  set("${output}" "${command_output}" PARENT_SCOPE)
  set("${error}" "${command_error}" PARENT_SCOPE)
  set("${result}" "${command_result}" PARENT_SCOPE)
endfunction()

run_conflict(status_output status_error status_result status --json)
if(NOT status_result EQUAL 0)
  message(FATAL_ERROR "status failed:\n${status_output}${status_error}")
endif()
string(JSON conflict_count LENGTH "${status_output}" conflicts)
if(NOT conflict_count EQUAL 1)
  message(FATAL_ERROR "status omitted the unresolved conflict: ${status_output}")
endif()

run_conflict(show_output show_error show_result conflict show document.txt)
if(NOT show_result EQUAL 0 OR NOT show_output MATCHES "kind=crdt-suspicious" OR
   NOT show_output MATCHES "path=document.txt" OR
   NOT show_output MATCHES "test double reported a suspicious CRDT merge")
  message(FATAL_ERROR "conflict show failed:\n${show_output}${show_error}")
endif()

run_conflict(resolve_output resolve_error resolve_result conflict resolve document.txt)
if(NOT resolve_result EQUAL 0 OR NOT resolve_output MATCHES "resolved=")
  message(FATAL_ERROR "conflict resolve failed:\n${resolve_output}${resolve_error}")
endif()
run_conflict(resolved_status resolved_error resolved_result status --json)
if(NOT resolved_result EQUAL 0)
  message(FATAL_ERROR "resolved status failed:\n${resolved_status}${resolved_error}")
endif()
string(JSON resolved_count LENGTH "${resolved_status}" conflicts)
if(NOT resolved_count EQUAL 0)
  message(FATAL_ERROR "resolved conflict remained in status: ${resolved_status}")
endif()

message(STATUS "${status_output}${show_output}${resolve_output}${resolved_status}")
