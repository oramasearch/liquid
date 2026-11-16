# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_LOCK_ROOT)
  message(FATAL_ERROR "lock CLI test arguments are required")
endif()

file(REMOVE_RECURSE "${LIQUID_LOCK_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_LOCK_ROOT}/generated")
file(WRITE "${LIQUID_LOCK_ROOT}/liquid.liquid"
  "lock generated { paths: [path \"generated/**\"] }\n")
file(WRITE "${LIQUID_LOCK_ROOT}/generated/archive.zip" "fixture\n")
set(identity_home "${LIQUID_LOCK_ROOT}/identity")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "LIQUID_HOME=${identity_home}"
    "LIQUID_DAEMON_IDLE_MS=5000"
    "${LIQUID_CLI}" init "${LIQUID_LOCK_ROOT}"
  RESULT_VARIABLE init_result
  OUTPUT_VARIABLE init_output
  ERROR_VARIABLE init_error)
if(NOT init_result EQUAL 0)
  message(FATAL_ERROR "repository init failed: ${init_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "LIQUID_HOME=${identity_home}"
    "LIQUID_DAEMON_IDLE_MS=5000"
    "${LIQUID_CLI}" lock generated/archive.zip --reason "regenerate archive"
  WORKING_DIRECTORY "${LIQUID_LOCK_ROOT}"
  RESULT_VARIABLE lock_result
  OUTPUT_VARIABLE lock_output
  ERROR_VARIABLE lock_error)
if(NOT lock_result EQUAL 0 OR NOT lock_output MATCHES "locked generated/archive.zip")
  message(FATAL_ERROR "lock failed: ${lock_output}${lock_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "LIQUID_HOME=${identity_home}"
    "${LIQUID_CLI}" unlock generated/archive.zip
  WORKING_DIRECTORY "${LIQUID_LOCK_ROOT}"
  RESULT_VARIABLE unlock_result
  OUTPUT_VARIABLE unlock_output
  ERROR_VARIABLE unlock_error)
if(NOT unlock_result EQUAL 0 OR NOT unlock_output MATCHES "unlocked generated/archive.zip")
  message(FATAL_ERROR "unlock failed: ${unlock_output}${unlock_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "LIQUID_HOME=${identity_home}"
    "${LIQUID_CLI}" daemon --shutdown
  WORKING_DIRECTORY "${LIQUID_LOCK_ROOT}")
file(REMOVE_RECURSE "${LIQUID_LOCK_ROOT}")
