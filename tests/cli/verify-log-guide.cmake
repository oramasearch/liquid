if(NOT DEFINED LIQUID_CLI)
  message(FATAL_ERROR "LIQUID_CLI is required")
endif()

if(NOT DEFINED LIQUID_TRANSPARENCY_GUIDE)
  message(FATAL_ERROR "LIQUID_TRANSPARENCY_GUIDE is required")
endif()

file(READ "${LIQUID_TRANSPARENCY_GUIDE}" guide)
set(documented_invocation "liquid verify --all --log <log-url>")
string(FIND "${guide}" "${documented_invocation}" invocation_offset)
if(invocation_offset EQUAL -1)
  message(FATAL_ERROR "transparency guide no longer documents ${documented_invocation}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" verify --all --log http://127.0.0.1:1
  WORKING_DIRECTORY "${LIQUID_GUIDE_WORKING_DIRECTORY}"
  RESULT_VARIABLE verify_result
  OUTPUT_VARIABLE verify_output
  ERROR_VARIABLE verify_error)
if(verify_result EQUAL 2 OR verify_error MATCHES "usage: verify does not accept --log")
  message(FATAL_ERROR
    "guide invocation was rejected by the CLI parser:\n${verify_output}${verify_error}")
endif()
