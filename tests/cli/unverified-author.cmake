# liquid#508: every command that prints an author shows one no identity chain verified as
# "signed by an unverified key", never as the name the record claims. The repository is written
# under one LIQUID_HOME and read under another that holds no chain for its author.
if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_UNVERIFIED_ROOT)
  message(FATAL_ERROR "LIQUID_CLI and LIQUID_UNVERIFIED_ROOT are required")
endif()

set(repository "${LIQUID_UNVERIFIED_ROOT}/repository")
set(author_home "${LIQUID_UNVERIFIED_ROOT}/author-home")
set(reader_home "${LIQUID_UNVERIFIED_ROOT}/reader-home")
set(unverified "signed by an unverified key")
file(REMOVE_RECURSE "${LIQUID_UNVERIFIED_ROOT}")
file(MAKE_DIRECTORY "${repository}" "${author_home}" "${reader_home}")
file(WRITE "${repository}/notes.txt" "first\nsecond\n")

function(run_liquid home output_variable)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "LIQUID_HOME=${home}" "${LIQUID_CLI}" ${ARGN}
    WORKING_DIRECTORY "${repository}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "liquid ${ARGN} failed: ${error}")
  endif()
  set(${output_variable} "${output}" PARENT_SCOPE)
endfunction()

function(shutdown_daemon home)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "LIQUID_HOME=${home}" "${LIQUID_CLI}" daemon --shutdown
    WORKING_DIRECTORY "${repository}"
    OUTPUT_QUIET
    ERROR_QUIET)
endfunction()

function(expect_unverified text label)
  string(FIND "${text}" "${unverified}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "${label} does not show an unverified author: ${text}")
  endif()
endfunction()

run_liquid("${author_home}" init_output init .)
run_liquid("${author_home}" cut_output cut initial)

# In the author's own home the author is verified.
run_liquid("${author_home}" author_ops ops --json)
if(NOT author_ops MATCHES "\"identity_verified\":true")
  message(FATAL_ERROR "ops --json does not verify the local author: ${author_ops}")
endif()
string(FIND "${author_ops}" "${unverified}" found)
if(NOT found EQUAL -1)
  message(FATAL_ERROR "ops --json shows the local author as unverified: ${author_ops}")
endif()
shutdown_daemon("${author_home}")

run_liquid("${reader_home}" log_json log --json)
expect_unverified("${log_json}" "log --json")
if(NOT log_json MATCHES "\"identity_verified\":false")
  message(FATAL_ERROR "log --json is missing identity_verified false: ${log_json}")
endif()

run_liquid("${reader_home}" history_text history notes.txt)
expect_unverified("${history_text}" "history")

run_liquid("${reader_home}" ops_json ops --json)
if(NOT ops_json MATCHES "\"actor\":\"signed by an unverified key\",\"identity_verified\":false")
  message(FATAL_ERROR "ops --json does not show an unverified actor: ${ops_json}")
endif()
run_liquid("${reader_home}" ops_text ops)
expect_unverified("${ops_text}" "ops")

run_liquid("${reader_home}" blame_json blame notes.txt --json)
if(NOT blame_json MATCHES "\"actor\":\"signed by an unverified key\",\"identity_verified\":false")
  message(FATAL_ERROR "blame --json does not show an unverified actor: ${blame_json}")
endif()
run_liquid("${reader_home}" blame_text blame notes.txt)
if(NOT blame_text MATCHES "actor=signed by an unverified key agent=false")
  message(FATAL_ERROR "blame does not show an unverified actor: ${blame_text}")
endif()
shutdown_daemon("${reader_home}")
