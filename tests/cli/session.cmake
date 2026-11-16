# SPDX-License-Identifier: MIT

# liquid#515: `liquid session start <delegation> --as <agent>` acts as the delegated agent, and
# history names that agent, not the principal. A delegation is refused for any other agent.
if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_SESSION_FIXTURE OR
   NOT DEFINED LIQUID_SESSION_ROOT)
  message(FATAL_ERROR "session CLI test requires the CLI, fixture, and root")
endif()

file(REMOVE_RECURSE "${LIQUID_SESSION_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_SESSION_ROOT}/work/src")
file(WRITE "${LIQUID_SESSION_ROOT}/work/src/value.txt" "one\n")
file(WRITE "${LIQUID_SESSION_ROOT}/agent-value.txt" "two\n")
set(work "${LIQUID_SESSION_ROOT}/work")
set(identity_home "${LIQUID_SESSION_ROOT}/identity")
set(agent "taap:1:ed25519:session-cli-agent")

function(run_in_work output_name)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "LIQUID_HOME=${identity_home}" ${ARGN}
    WORKING_DIRECTORY "${work}"
    RESULT_VARIABLE command_result
    OUTPUT_VARIABLE command_output
    ERROR_VARIABLE command_error)
  set(${output_name}_result "${command_result}" PARENT_SCOPE)
  set(${output_name} "${command_output}${command_error}" PARENT_SCOPE)
endfunction()

run_in_work(init "${LIQUID_CLI}" init .)
if(NOT init_result EQUAL 0)
  message(FATAL_ERROR "session fixture init failed: ${init}")
endif()
run_in_work(cut "${LIQUID_CLI}" cut initial)
if(NOT cut_result EQUAL 0)
  message(FATAL_ERROR "session fixture cut failed: ${cut}")
endif()
run_in_work(shutdown "${LIQUID_CLI}" daemon --shutdown)

run_in_work(issued "${LIQUID_SESSION_FIXTURE}" "${agent}")
if(NOT issued_result EQUAL 0)
  message(FATAL_ERROR "delegation fixture failed: ${issued}")
endif()
string(STRIP "${issued}" delegation)

run_in_work(missing "${LIQUID_CLI}" session start "${delegation}")
if(NOT missing_result EQUAL 2)
  message(FATAL_ERROR "session start without --as was not a usage error: ${missing}")
endif()

run_in_work(impostor "${LIQUID_CLI}" session start "${delegation}" --as "taap:1:ed25519:impostor")
if(impostor_result EQUAL 0 OR NOT impostor MATCHES "delegation was issued to another agent")
  message(FATAL_ERROR "session start accepted another agent: ${impostor}")
endif()

run_in_work(started "${LIQUID_CLI}" session start "${delegation}" --as "${agent}")
if(NOT started_result EQUAL 0)
  message(FATAL_ERROR "session start failed: ${started}")
endif()
string(STRIP "${started}" session)

run_in_work(written "${LIQUID_CLI}" session write "${session}" src/value.txt
            --from "${LIQUID_SESSION_ROOT}/agent-value.txt")
if(NOT written_result EQUAL 0)
  message(FATAL_ERROR "session write failed: ${written}")
endif()
run_in_work(published "${LIQUID_CLI}" session publish "${session}" --message agent)
if(NOT published_result EQUAL 0)
  message(FATAL_ERROR "session publish failed: ${published}")
endif()

run_in_work(blamed "${LIQUID_CLI}" blame src/value.txt)
if(NOT blamed_result EQUAL 0)
  message(FATAL_ERROR "blame after publish failed: ${blamed}")
endif()
# The fixture's agent has no identity chain on this machine, so history shows its records as
# signed by an unverified key rather than by the name they claim (liquid#511); the line is still
# the agent's, never the principal's.
if(NOT blamed MATCHES "actor=signed by an unverified key agent=true")
  message(FATAL_ERROR "published line is not attributed to the delegated agent: ${blamed}")
endif()

run_in_work(final_shutdown "${LIQUID_CLI}" daemon --shutdown)
file(REMOVE_RECURSE "${LIQUID_SESSION_ROOT}")
