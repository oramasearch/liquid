if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_DIFF_ROOT)
  message(FATAL_ERROR "LIQUID_CLI and LIQUID_DIFF_ROOT are required")
endif()

file(REMOVE_RECURSE "${LIQUID_DIFF_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_DIFF_ROOT}")
file(WRITE "${LIQUID_DIFF_ROOT}/notes.txt" "first\n")

function(run output)
  execute_process(
    COMMAND "${LIQUID_CLI}" ${ARGN}
    WORKING_DIRECTORY "${LIQUID_DIFF_ROOT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE command_output
    ERROR_VARIABLE command_error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "liquid ${ARGN} failed: ${command_output}${command_error}")
  endif()
  set("${output}" "${command_output}" PARENT_SCOPE)
endfunction()

run(ignored init .)
run(before_output cut before)
string(STRIP "${before_output}" before_output)
string(REPLACE "change=" "" before "${before_output}")
run(ignored rm notes.txt)
run(after_output cut after)
string(STRIP "${after_output}" after_output)
string(REPLACE "change=" "" after "${after_output}")
run(difference diff "${before}" "${after}")
string(FIND "${difference}" "removed\tnotes.txt" position)
if(position EQUAL -1)
  message(FATAL_ERROR "diff did not report the removed path: ${difference}")
endif()
