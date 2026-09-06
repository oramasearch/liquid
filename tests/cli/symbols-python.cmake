if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_SYMBOLS_PYTHON_ROOT)
  message(FATAL_ERROR "Python symbols test requires the CLI and fixture root")
endif()

file(REMOVE_RECURSE "${LIQUID_SYMBOLS_PYTHON_ROOT}")
file(MAKE_DIRECTORY "${LIQUID_SYMBOLS_PYTHON_ROOT}")
file(WRITE "${LIQUID_SYMBOLS_PYTHON_ROOT}/sample.py" "def answer():\n    return 42\n")

execute_process(
  COMMAND "${LIQUID_CLI}" init .
  WORKING_DIRECTORY "${LIQUID_SYMBOLS_PYTHON_ROOT}"
  RESULT_VARIABLE init_result
  OUTPUT_VARIABLE init_output
  ERROR_VARIABLE init_error)
if(NOT init_result EQUAL 0)
  message(FATAL_ERROR "Python fixture init failed: ${init_output}${init_error}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" symbols sample.py
  WORKING_DIRECTORY "${LIQUID_SYMBOLS_PYTHON_ROOT}"
  RESULT_VARIABLE symbols_result
  OUTPUT_VARIABLE symbols_output
  ERROR_VARIABLE symbols_error)
if(NOT symbols_result EQUAL 0)
  message(FATAL_ERROR "Python symbols failed: ${symbols_output}${symbols_error}")
endif()
if(NOT symbols_output MATCHES "origin=syntactic")
  message(FATAL_ERROR "Python symbols did not report syntactic origin: ${symbols_output}")
endif()
if(NOT symbols_output MATCHES "python:answer")
  message(FATAL_ERROR "Python tags query did not report answer: ${symbols_output}")
endif()
