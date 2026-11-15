# `liquid serve reapi`, `liquid serve sync` and `liquid scheduler` refuse a non-loopback listener unless it has TLS and
# client authentication (liquid#415, liquid#403). Every refusal happens before anything binds, so
# each command must exit at once; the timeout turns a regression that starts serving into a failure.
if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_SERVE_ROOT)
  message(FATAL_ERROR "serve listener test arguments are required")
endif()

file(REMOVE_RECURSE "${LIQUID_SERVE_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_SERVE_ROOT}/repository")
set(identity_home "${LIQUID_SERVE_ROOT}/identity")
set(token_file "${LIQUID_SERVE_ROOT}/token")
set(shared_token_file "${LIQUID_SERVE_ROOT}/shared-token")
file(WRITE "${token_file}" "serve-listen-test-token-0123456789abcdef\n")
file(CHMOD "${token_file}" PERMISSIONS OWNER_READ OWNER_WRITE)
file(WRITE "${shared_token_file}" "serve-listen-test-token-0123456789abcdef\n")
file(CHMOD "${shared_token_file}" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ)

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "LIQUID_HOME=${identity_home}"
    "LIQUID_DAEMON_IDLE_MS=5000"
    "${LIQUID_CLI}" init "${LIQUID_SERVE_ROOT}/repository"
  RESULT_VARIABLE init_result
  ERROR_VARIABLE init_error)
if(NOT init_result EQUAL 0)
  message(FATAL_ERROR "repository init failed: ${init_error}")
endif()

# Runs `liquid <arguments>` in the repository and requires a failure whose output matches `pattern`.
function(expect_refused name pattern)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      "LIQUID_HOME=${identity_home}"
      "LIQUID_OFFLINE=0"
      "${LIQUID_CLI}" ${ARGN}
    WORKING_DIRECTORY "${LIQUID_SERVE_ROOT}/repository"
    TIMEOUT 30
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT result MATCHES "^[0-9]+$")
    message(FATAL_ERROR "${name}: expected a refusal, got '${result}': ${output}${error}")
  endif()
  if(NOT "${output}${error}" MATCHES "${pattern}")
    message(FATAL_ERROR "${name}: expected '${pattern}', got: ${output}${error}")
  endif()
endfunction()

set(off_loopback "requires TLS and client authentication")

expect_refused("reapi plaintext on any address" "${off_loopback}"
  serve reapi --listen 0.0.0.0:50991)
expect_refused("reapi plaintext on an IPv6 address" "${off_loopback}"
  serve reapi --listen [::]:50991)
expect_refused("reapi token without TLS" "${off_loopback}"
  serve reapi --listen 0.0.0.0:50991 --token-file "${token_file}")
expect_refused("reapi TLS without authentication" "${off_loopback}"
  serve reapi --listen 0.0.0.0:50991 --tls-cert cert.pem --tls-key key.pem)
expect_refused("reapi group-readable token file" "readable only by its owner"
  serve reapi --listen 127.0.0.1:50991 --token-file "${shared_token_file}")
expect_refused("reapi certificate without key" "must be given together"
  serve reapi --listen 127.0.0.1:50991 --tls-cert cert.pem)

# liquid#520: the scheduler serves the same REAPI services and follows the same rules.
set(scheduler_store "${LIQUID_SERVE_ROOT}/scheduler-store")
expect_refused("scheduler plaintext on any address" "${off_loopback}"
  scheduler --listen 0.0.0.0:50993 --control-listen 127.0.0.1:50994 --store "${scheduler_store}")
expect_refused("scheduler plaintext on an IPv6 address" "${off_loopback}"
  scheduler --listen [::]:50993 --control-listen 127.0.0.1:50994 --store "${scheduler_store}")
expect_refused("scheduler token without TLS" "${off_loopback}"
  scheduler --listen 0.0.0.0:50993 --control-listen 127.0.0.1:50994 --store "${scheduler_store}"
  --token-file "${token_file}")
expect_refused("scheduler TLS without authentication" "${off_loopback}"
  scheduler --listen 0.0.0.0:50993 --control-listen 127.0.0.1:50994 --store "${scheduler_store}"
  --tls-cert cert.pem --tls-key key.pem)
expect_refused("scheduler group-readable token file" "readable only by its owner"
  scheduler --listen 127.0.0.1:50993 --control-listen 127.0.0.1:50994 --store "${scheduler_store}"
  --token-file "${shared_token_file}")

set(repository "${LIQUID_SERVE_ROOT}/repository")
expect_refused("sync plaintext on any address" "${off_loopback}"
  serve sync "${repository}" --listen 0.0.0.0 --port 50992)
expect_refused("sync plaintext on an IPv6 address" "${off_loopback}"
  serve sync "${repository}" --listen :: --port 50992)
expect_refused("sync token without TLS" "${off_loopback}"
  serve sync "${repository}" --listen 0.0.0.0 --port 50992 --token-file "${token_file}")
expect_refused("sync TLS without authentication" "${off_loopback}"
  serve sync "${repository}" --listen 0.0.0.0 --port 50992 --tls-cert cert.pem --tls-key key.pem)
expect_refused("sync group-readable token file" "readable only by its owner"
  serve sync "${repository}" --port 50992 --token-file "${shared_token_file}")
expect_refused("sync zero upload quota" "usage"
  serve sync "${repository}" --port 50992 --upload-quota 0)

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "LIQUID_HOME=${identity_home}"
    "${LIQUID_CLI}" daemon --shutdown
  WORKING_DIRECTORY "${LIQUID_SERVE_ROOT}/repository")
file(REMOVE_RECURSE "${LIQUID_SERVE_ROOT}")
