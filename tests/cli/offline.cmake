# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_OFFLINE_CHILD)
  foreach(required IN ITEMS
      LIQUID_OFFLINE_RUNNER
      LIQUID_OFFLINE_SANDBOX
      LIQUID_SOURCE_ROOT
      LIQUID_BUILD_ROOT
      LIQUID_CLI
      LIQUID_CTEST)
    if(NOT DEFINED ${required})
      message(FATAL_ERROR "${required} is required")
    endif()
  endforeach()
  file(REMOVE_RECURSE "${LIQUID_OFFLINE_SANDBOX}")
  execute_process(
    COMMAND "${LIQUID_OFFLINE_RUNNER}"
      "${LIQUID_OFFLINE_SANDBOX}"
      "${LIQUID_SOURCE_ROOT}"
      --
      "${CMAKE_COMMAND}"
      -DLIQUID_OFFLINE_CHILD=ON
      -DLIQUID_CLI=${LIQUID_CLI}
      -DLIQUID_CTEST=${LIQUID_CTEST}
      -DLIQUID_BUILD_ROOT=${LIQUID_BUILD_ROOT}
      -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE sandbox_result
    OUTPUT_VARIABLE sandbox_output
    ERROR_VARIABLE sandbox_error)
  if(sandbox_result EQUAL 77)
    # The runner could not create its namespaces here (liquid#363): ctest reports a skip
    # through the test's SKIP_REGULAR_EXPRESSION, with the runner's reason.
    string(STRIP "${sandbox_output}${sandbox_error}" skip_reason)
    message(STATUS "cli-offline skipped: ${skip_reason}")
    return()
  endif()
  if(NOT sandbox_result EQUAL 0)
    message(FATAL_ERROR
      "offline CLI suite failed with ${sandbox_result}:\n${sandbox_output}${sandbox_error}")
  endif()
  message(STATUS "offline CLI suite:\n${sandbox_output}${sandbox_error}")
  return()
endif()

foreach(required IN ITEMS LIQUID_CLI LIQUID_CTEST LIQUID_BUILD_ROOT)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

function(require_network_refusal reason verb)
  execute_process(
    COMMAND "${LIQUID_CLI}" "${verb}" ${ARGN}
    RESULT_VARIABLE command_result
    OUTPUT_VARIABLE command_output
    ERROR_VARIABLE command_error)
  string(STRIP "${command_error}" command_error)
  set(expected "liquid ${verb}: needs the network for ${reason}")
  if(NOT command_result EQUAL 69 OR NOT command_error STREQUAL expected OR command_output)
    message(FATAL_ERROR
      "${verb} offline refusal changed:\nresult=${command_result}\nstdout=${command_output}\nstderr=${command_error}")
  endif()
  message(STATUS "${command_error}")
endfunction()

require_network_refusal("push" push https://offline.invalid/repository)
require_network_refusal("pull" pull https://offline.invalid/repository)
require_network_refusal("remote execution" worker --scheduler offline.invalid:50051)
require_network_refusal(
  "the remote cache"
  wrap
  --remote https://offline.invalid
  --remote-publish
  true)

execute_process(
  COMMAND "${LIQUID_CTEST}"
    --test-dir "${LIQUID_BUILD_ROOT}"
    --output-on-failure
    --parallel 1
    --no-tests=error
    -L liquid-cli
    -E "^cli-offline$"
  RESULT_VARIABLE suite_result
  OUTPUT_VARIABLE suite_output
  ERROR_VARIABLE suite_error)
if(NOT suite_result EQUAL 0)
  message(FATAL_ERROR
    "sandboxed liquid-cli tests failed with ${suite_result}:\n${suite_output}${suite_error}")
endif()
message(STATUS "${suite_output}${suite_error}")
