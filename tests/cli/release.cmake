if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_RELEASE_ROOT)
  message(FATAL_ERROR "release CLI test requires the CLI and root")
endif()

file(REMOVE_RECURSE "${LIQUID_RELEASE_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_RELEASE_ROOT}/repo")
set(artifact "blake3:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")

execute_process(
  COMMAND "${LIQUID_CLI}" init "${LIQUID_RELEASE_ROOT}/repo"
  RESULT_VARIABLE init_result
  ERROR_VARIABLE init_error)
if(NOT init_result EQUAL 0)
  message(FATAL_ERROR "release CLI repository init failed: ${init_error}")
endif()

function(identity name output_name)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "LIQUID_HOME=${LIQUID_RELEASE_ROOT}/${name}"
            "${LIQUID_CLI}" identity show --json
    RESULT_VARIABLE identity_result
    OUTPUT_VARIABLE identity_output
    ERROR_VARIABLE identity_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT identity_result EQUAL 0)
    message(FATAL_ERROR "release CLI identity failed: ${identity_error}")
  endif()
  string(JSON identity_id GET "${identity_output}" id)
  set(${output_name} "${identity_id}" PARENT_SCOPE)
endfunction()

identity(first first_id)
identity(second second_id)

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "LIQUID_HOME=${LIQUID_RELEASE_ROOT}/policy"
          "${LIQUID_CLI}" release policy set --threshold 2
          --signer "${first_id}" --signer "${second_id}"
          --not-after 2099-01-01T00:00:00Z
  WORKING_DIRECTORY "${LIQUID_RELEASE_ROOT}/repo"
  RESULT_VARIABLE policy_result
  ERROR_VARIABLE policy_error)
if(NOT policy_result EQUAL 0)
  message(FATAL_ERROR "release policy command failed: ${policy_error}")
endif()

foreach(name IN ITEMS first second)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "LIQUID_HOME=${LIQUID_RELEASE_ROOT}/${name}"
            "${LIQUID_CLI}" release approve "${artifact}"
    WORKING_DIRECTORY "${LIQUID_RELEASE_ROOT}/repo"
    RESULT_VARIABLE approval_result
    ERROR_VARIABLE approval_error)
  if(NOT approval_result EQUAL 0)
    message(FATAL_ERROR "release approval command failed: ${approval_error}")
  endif()
endforeach()

# liquid#383: `first` verifies, and it binds a signer's name to a key only through an identity
# chain it holds. Until it trusts the policy issuer, the policy names an unknown signer.
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "LIQUID_HOME=${LIQUID_RELEASE_ROOT}/first"
          "${LIQUID_CLI}" release verify "${artifact}"
  WORKING_DIRECTORY "${LIQUID_RELEASE_ROOT}/repo"
  RESULT_VARIABLE unknown_policy_result
  OUTPUT_VARIABLE unknown_policy_output
  ERROR_VARIABLE unknown_policy_error)
if(unknown_policy_result EQUAL 0 OR NOT unknown_policy_error MATCHES "unknown signer")
  message(FATAL_ERROR
    "release verify accepted a policy from an unknown signer: ${unknown_policy_error}")
endif()

function(trust from_name into_name)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "LIQUID_HOME=${LIQUID_RELEASE_ROOT}/${from_name}"
            "${LIQUID_CLI}" identity show --chain
    RESULT_VARIABLE chain_result
    OUTPUT_VARIABLE chain_output
    ERROR_VARIABLE chain_error)
  if(NOT chain_result EQUAL 0 OR NOT chain_output MATCHES "^[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "identity show --chain failed: ${chain_error} ${chain_output}")
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "LIQUID_HOME=${LIQUID_RELEASE_ROOT}/${into_name}"
            "${LIQUID_CLI}" identity trust "${chain_output}" --json
    RESULT_VARIABLE trust_result
    OUTPUT_VARIABLE trust_output
    ERROR_VARIABLE trust_error)
  if(NOT trust_result EQUAL 0)
    message(FATAL_ERROR "identity trust failed: ${trust_error}")
  endif()
  string(JSON trusted_id GET "${trust_output}" id)
  set(trusted_id "${trusted_id}" PARENT_SCOPE)
endfunction()

trust(policy first)

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "LIQUID_HOME=${LIQUID_RELEASE_ROOT}/first"
          "${LIQUID_CLI}" release verify "${artifact}"
  WORKING_DIRECTORY "${LIQUID_RELEASE_ROOT}/repo"
  RESULT_VARIABLE unknown_approver_result
  OUTPUT_VARIABLE unknown_approver_output
  ERROR_VARIABLE unknown_approver_error)
if(unknown_approver_result EQUAL 0 OR
   NOT unknown_approver_output MATCHES "satisfied=false" OR
   NOT unknown_approver_output MATCHES "distinct_valid_signers=1" OR
   NOT unknown_approver_output MATCHES "unknown_signer=${second_id}")
  message(FATAL_ERROR
    "release verify counted an approval from an unknown signer: ${unknown_approver_output}")
endif()

trust(second first)
if(NOT trusted_id STREQUAL second_id)
  message(FATAL_ERROR "identity trust reported ${trusted_id}, expected ${second_id}")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "LIQUID_HOME=${LIQUID_RELEASE_ROOT}/first"
          "${LIQUID_CLI}" release verify "${artifact}"
  WORKING_DIRECTORY "${LIQUID_RELEASE_ROOT}/repo"
  RESULT_VARIABLE verify_result
  OUTPUT_VARIABLE verify_output
  ERROR_VARIABLE verify_error)
if(NOT verify_result EQUAL 0 OR
   NOT verify_output MATCHES "satisfied=true" OR
   NOT verify_output MATCHES "distinct_valid_signers=2")
  message(FATAL_ERROR "release verify command failed: ${verify_error} ${verify_output}")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "LIQUID_HOME=${LIQUID_RELEASE_ROOT}/first"
          "${LIQUID_CLI}" sign artifact "${artifact}"
  RESULT_VARIABLE sign_result
  OUTPUT_VARIABLE sign_output
  ERROR_VARIABLE sign_error)
if(NOT sign_result EQUAL 0 OR NOT sign_output MATCHES "signature=" OR
   NOT sign_output MATCHES "signer=${first_id}")
  message(FATAL_ERROR "sign command failed: ${sign_error} ${sign_output}")
endif()
