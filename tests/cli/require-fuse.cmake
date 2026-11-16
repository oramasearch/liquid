# Runs a CLI test that mounts FUSE, or reports it skipped when this host cannot (liquid#362).
#
#   cmake -DLIQUID_TEST_NAME=<name> [-DLIQUID_FUSE_PROBE=<liquid-fuse-probe>]
#         -P require-fuse.cmake -- <command> [args...]
#
# With a probe that exits 77 the test prints "-- <name> skipped: <reason>", which the test's
# SKIP_REGULAR_EXPRESSION turns into a skip. Otherwise the command runs in the current
# directory, its output is printed for the test's own pass expression, and a non-zero exit
# fails the test.
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED LIQUID_TEST_NAME)
  message(FATAL_ERROR "LIQUID_TEST_NAME is required")
endif()
set(command)
set(collecting OFF)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(index RANGE ${last})
  if(collecting)
    list(APPEND command "${CMAKE_ARGV${index}}")
  elseif(CMAKE_ARGV${index} STREQUAL "--")
    set(collecting ON)
  endif()
endforeach()
if(NOT command)
  message(FATAL_ERROR "expected -- <command> [args...]")
endif()

if(DEFINED LIQUID_FUSE_PROBE)
  execute_process(
    COMMAND "${LIQUID_FUSE_PROBE}"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_error)
  if(probe_result EQUAL 77)
    string(STRIP "${probe_output}${probe_error}" reason)
    message(STATUS "${LIQUID_TEST_NAME} skipped: ${reason}")
    return()
  endif()
  if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "FUSE probe failed with ${probe_result}:\n${probe_output}${probe_error}")
  endif()
endif()

execute_process(
  COMMAND ${command}
  RESULT_VARIABLE command_result
  OUTPUT_VARIABLE command_output
  ERROR_VARIABLE command_error)
message("${command_output}${command_error}")
if(NOT command_result EQUAL 0)
  message(FATAL_ERROR "${LIQUID_TEST_NAME} exited ${command_result}")
endif()
