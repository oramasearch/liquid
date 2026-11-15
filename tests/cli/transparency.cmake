# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI)
  message(FATAL_ERROR "LIQUID_CLI is required")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    --unset=LIQUID_LOG_URL
    --unset=LIQUID_TSA_TRUST_CERT
    "${LIQUID_CLI}" log verify --timestamps
  RESULT_VARIABLE missing_url_result
  OUTPUT_VARIABLE missing_url_output
  ERROR_VARIABLE missing_url_error)
if(missing_url_result EQUAL 0 OR
   NOT missing_url_error MATCHES "LIQUID_LOG_URL is required")
  message(FATAL_ERROR
    "timestamp verification did not refuse a missing log URL: ${missing_url_output}${missing_url_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    LIQUID_LOG_URL=https://lake.example
    --unset=LIQUID_TSA_TRUST_CERT
    "${LIQUID_CLI}" log verify --timestamps
  RESULT_VARIABLE missing_trust_result
  OUTPUT_VARIABLE missing_trust_output
  ERROR_VARIABLE missing_trust_error)
if(missing_trust_result EQUAL 0 OR
   NOT missing_trust_error MATCHES "no TSA trust anchors are configured")
  message(FATAL_ERROR
    "timestamp verification did not refuse empty trust: ${missing_trust_output}${missing_trust_error}")
endif()
