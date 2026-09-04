if(NOT EXISTS "${LIQUID_REALWORLD_GIT_DIRECTORY}/.git")
  message(FATAL_ERROR "LIQUID_REALWORLD_GIT_DIRECTORY must contain a Git checkout")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" import git "${LIQUID_REALWORLD_GIT_DIRECTORY}"
  RESULT_VARIABLE import_result
  OUTPUT_VARIABLE import_output
  ERROR_VARIABLE import_error)
if(NOT import_result EQUAL 0)
  message(FATAL_ERROR "Git import failed: ${import_output}${import_error}")
endif()

string(REGEX MATCH "peak_anonymous_bytes=([0-9]+)" peak_match "${import_output}")
if(NOT peak_match)
  message(FATAL_ERROR "Git import did not report peak anonymous memory: ${import_output}")
endif()
if(CMAKE_MATCH_1 GREATER_EQUAL 1073741824)
  message(FATAL_ERROR "Git import exceeded 1 GiB peak anonymous memory: ${CMAKE_MATCH_1}")
endif()

execute_process(
  COMMAND git symbolic-ref --short HEAD
  WORKING_DIRECTORY "${LIQUID_REALWORLD_GIT_DIRECTORY}"
  RESULT_VARIABLE ref_result
  OUTPUT_VARIABLE git_ref
  ERROR_VARIABLE ref_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT ref_result EQUAL 0)
  message(FATAL_ERROR "Cannot determine Git head ref: ${ref_error}")
endif()

set(export_directory "${LIQUID_REALWORLD_GIT_DIRECTORY}/.liquid-git-export")
file(REMOVE_RECURSE "${export_directory}")
execute_process(
  COMMAND "${LIQUID_CLI}" export git "${export_directory}" "${git_ref}"
  WORKING_DIRECTORY "${LIQUID_REALWORLD_GIT_DIRECTORY}"
  RESULT_VARIABLE export_result
  OUTPUT_VARIABLE export_output
  ERROR_VARIABLE export_error)
if(NOT export_result EQUAL 0)
  message(FATAL_ERROR "Git export failed: ${export_output}${export_error}")
endif()
execute_process(
  COMMAND git -C "${export_directory}" fsck --no-dangling
  RESULT_VARIABLE fsck_result
  OUTPUT_VARIABLE fsck_output
  ERROR_VARIABLE fsck_error)
if(NOT fsck_result EQUAL 0)
  message(FATAL_ERROR "Exported Git fsck failed: ${fsck_output}${fsck_error}")
endif()

execute_process(
  COMMAND du -sk "${LIQUID_REALWORLD_GIT_DIRECTORY}/.git" "${LIQUID_REALWORLD_GIT_DIRECTORY}/.liquid-store"
  RESULT_VARIABLE size_result
  OUTPUT_VARIABLE size_output
  ERROR_VARIABLE size_error)
if(NOT size_result EQUAL 0)
  message(FATAL_ERROR "Cannot measure Git and liquid storage: ${size_error}")
endif()
string(REGEX MATCHALL "([0-9]+)" size_values "${size_output}")
list(GET size_values 0 git_size_kib)
list(GET size_values 1 liquid_size_kib)
math(EXPR allowed_liquid_size_kib "${git_size_kib} * 3")
if(liquid_size_kib GREATER allowed_liquid_size_kib)
  message(FATAL_ERROR "Liquid storage exceeds 3x Git storage: ${size_output}")
endif()
message("Git import: ${import_output}")
message("Git export: ${export_output}")
message("Storage KiB: ${size_output}")
