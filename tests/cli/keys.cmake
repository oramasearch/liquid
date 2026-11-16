# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_KEYS_FIXTURE OR
   NOT DEFINED LIQUID_KEYS_ROOT)
  message(FATAL_ERROR "keys CLI test requires the CLI, fixture, and root")
endif()

set(passphrase "keys-cli-passphrase")
file(REMOVE_RECURSE "${LIQUID_KEYS_ROOT}")

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "LIQUID_PASSPHRASE=${passphrase}"
          "${LIQUID_KEYS_FIXTURE}" "${LIQUID_KEYS_ROOT}"
  RESULT_VARIABLE fixture_result)
if(NOT fixture_result EQUAL 0)
  message(FATAL_ERROR "keys CLI fixture failed")
endif()

function(run_keys output_name)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "LIQUID_PASSPHRASE=${passphrase}"
            "${LIQUID_CLI}" keys ${ARGN}
    WORKING_DIRECTORY "${LIQUID_KEYS_ROOT}"
    RESULT_VARIABLE command_result
    OUTPUT_VARIABLE command_output
    ERROR_VARIABLE command_error)
  if(NOT command_result EQUAL 0)
    message(FATAL_ERROR "liquid keys ${ARGN} failed: ${command_error}")
  endif()
  set(${output_name} "${command_output}" PARENT_SCOPE)
endfunction()

run_keys(show_output show private)
if(NOT show_output MATCHES "private@1 custody=passphrase current=true revoked=false")
  message(FATAL_ERROR "keys show did not describe the initial key: ${show_output}")
endif()

run_keys(rotate_output rotate private)
if(NOT rotate_output MATCHES "rotated private from version 1 to 2" OR
   NOT rotate_output MATCHES "Rotation does not rewrite history")
  message(FATAL_ERROR "keys rotate report was incomplete: ${rotate_output}")
endif()

run_keys(revoke_output revoke private 1)
if(NOT revoke_output MATCHES "revoked private@1")
  message(FATAL_ERROR "keys revoke report was incomplete: ${revoke_output}")
endif()

run_keys(rewrap_output rewrap private --to passphrase:)
if(NOT rewrap_output MATCHES "rewrapped private")
  message(FATAL_ERROR "keys rewrap report was incomplete: ${rewrap_output}")
endif()

run_keys(split_output split private --threshold 2 --shares 3)
string(REGEX MATCHALL "liquid-share-1-[^\n]+" split_lines "${split_output}")
list(LENGTH split_lines split_count)
if(NOT split_count EQUAL 3)
  message(FATAL_ERROR "keys split did not print three shares: ${split_output}")
endif()

run_keys(final_output show private)
if(NOT final_output MATCHES "private@2 custody=shamir current=true revoked=false")
  message(FATAL_ERROR "keys show did not describe Shamir custody: ${final_output}")
endif()
