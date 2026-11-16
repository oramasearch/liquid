# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI)
  message(FATAL_ERROR "LIQUID_CLI is required")
endif()
if(NOT DEFINED LIQUID_REFERENCE_SOURCE)
  message(FATAL_ERROR "LIQUID_REFERENCE_SOURCE is required")
endif()
if(NOT DEFINED LIQUID_REFERENCE_OUTPUT)
  message(FATAL_ERROR "LIQUID_REFERENCE_OUTPUT is required")
endif()

file(GLOB_RECURSE expected_files
  RELATIVE "${LIQUID_REFERENCE_SOURCE}"
  "${LIQUID_REFERENCE_SOURCE}/*")
file(GLOB_RECURSE actual_files
  RELATIVE "${LIQUID_REFERENCE_OUTPUT}"
  "${LIQUID_REFERENCE_OUTPUT}/*")
list(SORT expected_files)
list(SORT actual_files)
if(NOT expected_files STREQUAL actual_files)
  message(FATAL_ERROR
    "generated CLI reference file list is stale\nRefresh with: ${LIQUID_CLI} docs --format markdown --out ${LIQUID_REFERENCE_SOURCE} && ${LIQUID_CLI} docs --format json --out ${LIQUID_REFERENCE_SOURCE}\nexpected: ${expected_files}\nactual: ${actual_files}")
endif()

foreach(path IN LISTS expected_files)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
      "${LIQUID_REFERENCE_SOURCE}/${path}"
      "${LIQUID_REFERENCE_OUTPUT}/${path}"
    RESULT_VARIABLE comparison)
  if(NOT comparison EQUAL 0)
    message(FATAL_ERROR
      "generated CLI reference is stale: ${path}\nRefresh with: ${LIQUID_CLI} docs --format markdown --out ${LIQUID_REFERENCE_SOURCE} && ${LIQUID_CLI} docs --format json --out ${LIQUID_REFERENCE_SOURCE}")
  endif()
endforeach()
