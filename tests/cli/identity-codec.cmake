# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_IDENTITY_CODEC OR
   NOT DEFINED LIQUID_IDENTITY_HOME)
  message(FATAL_ERROR "cli identity codec test requires its executables and identity home")
endif()

find_program(identity_python NAMES python3 python REQUIRED)
function(check_document_output)
  execute_process(
    COMMAND "${identity_python}" "${CMAKE_CURRENT_LIST_DIR}/identity-document.py"
            "${LIQUID_CLI}" "${LIQUID_IDENTITY_CODEC}" "${LIQUID_IDENTITY_HOME}"
    RESULT_VARIABLE document_result)
  if(NOT document_result EQUAL 0)
    message(FATAL_ERROR "public identity document output failed verification")
  endif()
endfunction()

file(REMOVE_RECURSE "${LIQUID_IDENTITY_HOME}")
check_document_output()
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "LIQUID_HOME=${LIQUID_IDENTITY_HOME}"
          "${LIQUID_CLI}" identity show --json
  RESULT_VARIABLE show_result
  OUTPUT_VARIABLE show_output
  ERROR_VARIABLE show_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT show_result EQUAL 0)
  message(FATAL_ERROR "liquid identity show failed: ${show_error}")
endif()
string(JSON identity_fingerprint GET "${show_output}" fingerprint)
set(identity_root "${LIQUID_IDENTITY_HOME}/identity")
set(current_document "${identity_root}/${identity_fingerprint}.identity.cbor")
execute_process(
  COMMAND "${LIQUID_IDENTITY_CODEC}" "${current_document}"
  RESULT_VARIABLE initial_codec_result)
if(NOT initial_codec_result EQUAL 0)
  message(FATAL_ERROR "public codec rejected the initial identity document")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "LIQUID_HOME=${LIQUID_IDENTITY_HOME}"
          "${LIQUID_CLI}" identity rotate --json
  RESULT_VARIABLE rotate_result
  ERROR_VARIABLE rotate_error)
if(NOT rotate_result EQUAL 0)
  message(FATAL_ERROR "liquid identity rotate failed: ${rotate_error}")
endif()
set(document_root "${identity_root}/${identity_fingerprint}/documents")
execute_process(
  COMMAND "${LIQUID_IDENTITY_CODEC}"
          "${document_root}/000.cbor"
          "${document_root}/001.cbor"
  RESULT_VARIABLE rotation_codec_result)
if(NOT rotation_codec_result EQUAL 0)
  message(FATAL_ERROR "public codec rejected the identity rotation chain")
endif()

check_document_output()
