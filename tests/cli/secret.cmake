# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI)
  message(FATAL_ERROR "LIQUID_CLI is required")
endif()
if(NOT DEFINED LIQUID_SECRET_ROOT)
  message(FATAL_ERROR "LIQUID_SECRET_ROOT is required")
endif()

file(REMOVE_RECURSE "${LIQUID_SECRET_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_SECRET_ROOT}")
set(private_key "${LIQUID_SECRET_ROOT}/private.key")
set(value_file "${LIQUID_SECRET_ROOT}/value.txt")
set(record_file "${LIQUID_SECRET_ROOT}/record.cbor")
set(secret_value "cli secret value must stay off stderr")
set(public_key "86517bc0c39fe876b7646cb5e34ee03daf71ccbad67450c16a970ad8cbc03e7e")
file(WRITE "${private_key}" "0123456789abcdef0123456789abcdef")
file(CHMOD "${private_key}" PERMISSIONS OWNER_READ OWNER_WRITE)
file(WRITE "${value_file}" "${secret_value}")

execute_process(
  COMMAND "${LIQUID_CLI}" secret seal
    --public-key "${public_key}"
    --kid lake-key-2026-09
    --cluster lake-prod
    --namespace payments
    --name database
    --value-file "${value_file}"
    --out "${record_file}"
  RESULT_VARIABLE raw_seal_result
  ERROR_VARIABLE raw_seal_error)
if(NOT raw_seal_result EQUAL 0)
  message(FATAL_ERROR "raw secret seal failed: ${raw_seal_error}")
endif()
string(FIND "${raw_seal_error}" "${secret_value}" raw_seal_leak)
if(NOT raw_seal_leak EQUAL -1)
  message(FATAL_ERROR "raw secret seal wrote the value to stderr")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" secret unseal
    --private-key-file "${private_key}"
    --cluster lake-prod
    --namespace payments
    --name database
    "${record_file}"
  RESULT_VARIABLE raw_unseal_result
  OUTPUT_VARIABLE raw_plaintext
  ERROR_VARIABLE raw_unseal_error)
if(NOT raw_unseal_result EQUAL 0)
  message(FATAL_ERROR "raw secret unseal failed: ${raw_unseal_error}")
endif()
if(NOT raw_plaintext STREQUAL secret_value)
  message(FATAL_ERROR "raw secret round trip changed the value")
endif()
string(FIND "${raw_unseal_error}" "${secret_value}" raw_unseal_leak)
if(NOT raw_unseal_leak EQUAL -1)
  message(FATAL_ERROR "raw secret unseal wrote the value to stderr")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" secret seal
    --public-key "${public_key}"
    --kid lake-key-2026-09
    --cluster lake-prod
    --value-file "${value_file}"
  RESULT_VARIABLE base64_seal_result
  OUTPUT_VARIABLE base64_record
  ERROR_VARIABLE base64_seal_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT base64_seal_result EQUAL 0)
  message(FATAL_ERROR "base64 secret seal failed: ${base64_seal_error}")
endif()
string(FIND "${base64_seal_error}" "${secret_value}" base64_seal_leak)
if(NOT base64_seal_leak EQUAL -1)
  message(FATAL_ERROR "base64 secret seal wrote the value to stderr")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" secret unseal
    --private-key-file "${private_key}"
    --cluster lake-prod
    "${base64_record}"
  RESULT_VARIABLE base64_unseal_result
  OUTPUT_VARIABLE base64_plaintext
  ERROR_VARIABLE base64_unseal_error)
if(NOT base64_unseal_result EQUAL 0)
  message(FATAL_ERROR "base64 secret unseal failed: ${base64_unseal_error}")
endif()
if(NOT base64_plaintext STREQUAL secret_value)
  message(FATAL_ERROR "base64 secret round trip changed the value")
endif()
string(FIND "${base64_unseal_error}" "${secret_value}" base64_unseal_leak)
if(NOT base64_unseal_leak EQUAL -1)
  message(FATAL_ERROR "base64 secret unseal wrote the value to stderr")
endif()
