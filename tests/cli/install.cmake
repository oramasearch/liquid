if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_INSTALL_ROOT)
  message(FATAL_ERROR "LIQUID_CLI and LIQUID_INSTALL_ROOT are required")
endif()

set(source_one "${LIQUID_INSTALL_ROOT}-source-one")
set(source_two "${LIQUID_INSTALL_ROOT}-source-two")
file(REMOVE_RECURSE "${LIQUID_INSTALL_ROOT}" "${source_one}" "${source_two}")
file(MAKE_DIRECTORY "${source_one}/bin" "${source_two}/bin")
file(WRITE "${source_one}/bin/tool" "first\n")
file(WRITE "${source_two}/bin/tool" "second\n")

execute_process(
  COMMAND "${LIQUID_CLI}" install "${source_one}" --root "${LIQUID_INSTALL_ROOT}"
  RESULT_VARIABLE install_one_result
  OUTPUT_VARIABLE install_one_output
  ERROR_VARIABLE install_one_error)
if(NOT install_one_result EQUAL 0 OR NOT install_one_output MATCHES "generation=1 current=true")
  message(FATAL_ERROR "first install failed: ${install_one_error}${install_one_output}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" install "${source_two}" --root "${LIQUID_INSTALL_ROOT}"
  RESULT_VARIABLE install_two_result
  OUTPUT_VARIABLE install_two_output
  ERROR_VARIABLE install_two_error)
if(NOT install_two_result EQUAL 0 OR NOT install_two_output MATCHES "generation=2 current=true")
  message(FATAL_ERROR "second install failed: ${install_two_error}${install_two_output}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" install --list --root "${LIQUID_INSTALL_ROOT}"
  RESULT_VARIABLE list_result
  OUTPUT_VARIABLE list_output
  ERROR_VARIABLE list_error)
if(NOT list_result EQUAL 0)
  message(FATAL_ERROR "generation list failed: ${list_error}${list_output}")
endif()
if(NOT list_output MATCHES "generation=1 current=false")
  message(FATAL_ERROR "generation one missing from list: ${list_output}")
endif()
if(NOT list_output MATCHES "generation=2 current=true")
  message(FATAL_ERROR "generation two is not current: ${list_output}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" rollback --root "${LIQUID_INSTALL_ROOT}"
  RESULT_VARIABLE rollback_result
  OUTPUT_VARIABLE rollback_output
  ERROR_VARIABLE rollback_error)
if(NOT rollback_result EQUAL 0 OR NOT rollback_output MATCHES "generation=1 current=true")
  message(FATAL_ERROR "rollback failed: ${rollback_error}${rollback_output}")
endif()

file(READ "${LIQUID_INSTALL_ROOT}/current/bin/tool" current_contents)
if(NOT current_contents STREQUAL "first\n")
  message(FATAL_ERROR "rollback exposed unexpected contents: ${current_contents}")
endif()
