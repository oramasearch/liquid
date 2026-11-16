# SPDX-License-Identifier: MIT

# liquid #416: the scheduler and the worker refuse to start without the identity they must
# authenticate, and load the documents `liquid identity show --document` prints.
foreach(required IN ITEMS LIQUID_CLI LIQUID_WORKERS_TRUST_WORK)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${LIQUID_WORKERS_TRUST_WORK}")
file(MAKE_DIRECTORY "${LIQUID_WORKERS_TRUST_WORK}/home" "${LIQUID_WORKERS_TRUST_WORK}/workers")
set(ENV{LIQUID_HOME} "${LIQUID_WORKERS_TRUST_WORK}/home")
unset(ENV{LIQUID_TRUSTED_WORKERS})
unset(ENV{LIQUID_TRUSTED_SCHEDULER})

function(expect_refusal expected_result expected_text)
  execute_process(
    COMMAND "${LIQUID_CLI}" ${ARGN}
    RESULT_VARIABLE command_result
    OUTPUT_VARIABLE command_output
    ERROR_VARIABLE command_error
    TIMEOUT 30)
  string(FIND "${command_error}" "${expected_text}" found)
  if(NOT command_result STREQUAL "${expected_result}" OR found EQUAL -1)
    message(FATAL_ERROR
      "liquid ${ARGN}: expected ${expected_result} and '${expected_text}'\n"
      "result=${command_result}\nstdout=${command_output}\nstderr=${command_error}")
  endif()
  message(STATUS "liquid ${ARGN}: ${command_error}")
endfunction()

expect_refusal(2 "--trust-worker" scheduler --listen 127.0.0.1:50051)
expect_refusal(2 "--trust-scheduler" worker --scheduler 127.0.0.1:50051)

file(WRITE "${LIQUID_WORKERS_TRUST_WORK}/garbage" "not an identity document\n")
expect_refusal(1 "identity document is not base64url"
  scheduler --listen 127.0.0.1:50051 --trust-worker "${LIQUID_WORKERS_TRUST_WORK}/garbage")
expect_refusal(1 "identity document is not base64url"
  worker --scheduler 127.0.0.1:50051 --trust-scheduler "${LIQUID_WORKERS_TRUST_WORK}/garbage")

execute_process(
  COMMAND "${LIQUID_CLI}" identity show --document
  RESULT_VARIABLE show_result
  OUTPUT_FILE "${LIQUID_WORKERS_TRUST_WORK}/workers/worker-one"
  ERROR_VARIABLE show_error)
if(NOT show_result STREQUAL "0")
  message(FATAL_ERROR "identity show --document failed (${show_result}): ${show_error}")
endif()
# A directory of documents is accepted, dot files are skipped, and the scheduler gets as far as
# its listener: port zero is refused only after every document has verified.
file(WRITE "${LIQUID_WORKERS_TRUST_WORK}/workers/.hidden" "ignored")
expect_refusal(1 "REAPI listen port must be non-zero"
  scheduler --listen 127.0.0.1:0 --control-listen 127.0.0.1:0
  --trust-worker "${LIQUID_WORKERS_TRUST_WORK}/workers")
