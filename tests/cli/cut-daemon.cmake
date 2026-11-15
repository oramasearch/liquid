# liquid#528: `cut -m` records only the message, a message starting with `-` is refused, and
# `daemon stop` / `daemon --shutdown` with no daemon running says so instead of a socket error.
if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_CLI_ROOT)
  message(FATAL_ERROR "LIQUID_CLI and LIQUID_CLI_ROOT are required")
endif()

set(workspace "${LIQUID_CLI_ROOT}/workspace")
set(identity "${LIQUID_CLI_ROOT}/identity")

function(run_liquid output error result)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "LIQUID_HOME=${identity}" "LIQUID_DAEMON_IDLE_MS=5000"
      "${LIQUID_CLI}" ${ARGN}
    WORKING_DIRECTORY "${workspace}"
    RESULT_VARIABLE command_result
    OUTPUT_VARIABLE command_output
    ERROR_VARIABLE command_error)
  set("${output}" "${command_output}" PARENT_SCOPE)
  set("${error}" "${command_error}" PARENT_SCOPE)
  set("${result}" "${command_result}" PARENT_SCOPE)
endfunction()

if(EXISTS "${workspace}")
  run_liquid(ignored_output ignored_error ignored_result daemon --shutdown)
endif()
file(REMOVE_RECURSE "${LIQUID_CLI_ROOT}")
file(MAKE_DIRECTORY "${workspace}")
file(WRITE "${workspace}/first.txt" "first\n")

run_liquid(init_output init_error init_result init .)
if(NOT init_result EQUAL 0)
  message(FATAL_ERROR "init failed: ${init_output}${init_error}")
endif()

# With no daemon running, both spellings of stop say so and succeed.
run_liquid(stop_output stop_error stop_result daemon --shutdown)
if(NOT stop_result EQUAL 0 OR NOT stop_output MATCHES "no daemon is running" OR
   stop_error MATCHES "unavailable")
  message(FATAL_ERROR "daemon --shutdown without a daemon: ${stop_result} ${stop_output}${stop_error}")
endif()
run_liquid(stop_output stop_error stop_result daemon stop)
if(NOT stop_result EQUAL 0 OR NOT stop_output MATCHES "no daemon is running")
  message(FATAL_ERROR "daemon stop without a daemon: ${stop_result} ${stop_output}${stop_error}")
endif()
run_liquid(bad_output bad_error bad_result daemon halt)
if(NOT bad_result EQUAL 2 OR NOT bad_error MATCHES "daemon \\[start \\| stop \\| --shutdown\\]")
  message(FATAL_ERROR "daemon halt was not refused: ${bad_result} ${bad_output}${bad_error}")
endif()

# `daemon start` starts it, and `daemon stop` stops it.
run_liquid(start_output start_error start_result daemon start)
if(NOT start_result EQUAL 0 OR NOT start_output MATCHES "running=true")
  message(FATAL_ERROR "daemon start failed: ${start_result} ${start_output}${start_error}")
endif()
run_liquid(stop_output stop_error stop_result daemon stop)
if(NOT stop_result EQUAL 0 OR stop_output MATCHES "no daemon is running")
  message(FATAL_ERROR "daemon stop after start: ${stop_result} ${stop_output}${stop_error}")
endif()

# `cut -m` and `cut --message` record only the message.
run_liquid(cut_output cut_error cut_result cut -m "Start the sample")
if(NOT cut_result EQUAL 0)
  message(FATAL_ERROR "cut -m failed: ${cut_output}${cut_error}")
endif()
file(APPEND "${workspace}/first.txt" "more\n")
run_liquid(write_output write_error write_result write first.txt --from "${workspace}/first.txt")
run_liquid(cut_output cut_error cut_result cut --message "Second change")
if(NOT cut_result EQUAL 0)
  message(FATAL_ERROR "cut --message failed: ${cut_output}${cut_error}")
endif()
run_liquid(words_output words_error words_result cut third change words)
if(NOT words_result EQUAL 0)
  message(FATAL_ERROR "cut <words> failed: ${words_output}${words_error}")
endif()
run_liquid(log_output log_error log_result log --json)
if(NOT log_result EQUAL 0)
  message(FATAL_ERROR "log failed: ${log_output}${log_error}")
endif()
string(JSON log_count LENGTH "${log_output}")
set(messages "")
math(EXPR last "${log_count} - 1")
foreach(index RANGE ${last})
  string(JSON entry GET "${log_output}" ${index} message)
  list(APPEND messages "${entry}")
endforeach()
foreach(expected "Start the sample" "Second change" "third change words")
  if(NOT expected IN_LIST messages)
    message(FATAL_ERROR "log is missing '${expected}': ${log_output}")
  endif()
endforeach()
if(log_output MATCHES "-m ")
  message(FATAL_ERROR "cut kept the flag in the message: ${log_output}")
endif()

# A message that starts with `-` is refused, whichever way it is given, and records nothing.
foreach(arguments "-x;oops" "--;-m;oops" "-m;-starts with a dash" "-m")
  run_liquid(refused_output refused_error refused_result cut ${arguments})
  if(NOT refused_result EQUAL 2)
    message(FATAL_ERROR "cut ${arguments} was not refused: ${refused_result} ${refused_output}${refused_error}")
  endif()
endforeach()
run_liquid(after_output after_error after_result log --json)
string(JSON after_count LENGTH "${after_output}")
if(NOT after_count EQUAL log_count)
  message(FATAL_ERROR "a refused cut recorded a change: ${after_output}")
endif()

run_liquid(ignored_output ignored_error ignored_result daemon stop)
file(REMOVE_RECURSE "${LIQUID_CLI_ROOT}")
