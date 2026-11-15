# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_WRAP_TIER_ROOT OR
   NOT DEFINED CMAKE_COMMAND)
  message(FATAL_ERROR "wrap tier test arguments are required")
endif()

file(REMOVE_RECURSE "${LIQUID_WRAP_TIER_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_WRAP_TIER_ROOT}")

execute_process(
  COMMAND "${LIQUID_CLI}" wrap "${CMAKE_COMMAND}" -E true
  WORKING_DIRECTORY "${LIQUID_WRAP_TIER_ROOT}"
  RESULT_VARIABLE wrap_result
  OUTPUT_VARIABLE wrap_output
  ERROR_VARIABLE wrap_error)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  set(expected "tier: 1 (wrap: Linux namespaces)")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(expected "tier: 1 (wrap: macOS sandbox-exec)")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(expected "tier: 1 (wrap: Windows unsupported)")
else()
  set(expected "tier: 1 (wrap: platform unsupported)")
endif()
string(FIND "${wrap_output}" "${expected}" position)
if(position EQUAL -1)
  message(FATAL_ERROR "wrap omitted its tier line: ${wrap_output}${wrap_error}")
endif()
