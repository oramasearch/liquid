if(NOT DEFINED GIT_EXECUTABLE OR NOT DEFINED FIXTURE OR NOT DEFINED MEASURE OR NOT DEFINED ROOT)
  message(FATAL_ERROR "merge measurement arguments are missing")
endif()
file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}/git")
execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${ROOT}/git"
  RESULT_VARIABLE initialized)
if(NOT initialized EQUAL 0)
  message(FATAL_ERROR "fixture git init failed")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${ROOT}/git" symbolic-ref HEAD refs/heads/main
  RESULT_VARIABLE selected)
if(NOT selected EQUAL 0)
  message(FATAL_ERROR "fixture branch selection failed")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${ROOT}/git" fast-import --quiet
  INPUT_FILE "${FIXTURE}"
  RESULT_VARIABLE imported)
if(NOT imported EQUAL 0)
  message(FATAL_ERROR "fixture fast-import failed")
endif()
execute_process(COMMAND "${MEASURE}" "${ROOT}/git" "${ROOT}/liquid"
  RESULT_VARIABLE measured
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(NOT measured EQUAL 0)
  message(FATAL_ERROR "merge measurement failed: ${error}")
endif()
if(NOT output MATCHES ",1,1,0,0,0,")
  message(FATAL_ERROR "unexpected merge measurement: ${output}")
endif()
message(STATUS "${output}")
