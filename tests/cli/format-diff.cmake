if(NOT DEFINED LIQUID_CLI OR NOT DEFINED LIQUID_FORMAT_DIFF_FIXTURE OR
   NOT DEFINED LIQUID_FORMAT_DIFF_ROOT OR NOT DEFINED LIQUID_FORMAT_FIXTURES)
  message(FATAL_ERROR "format diff test arguments are required")
endif()

file(REMOVE_RECURSE "${LIQUID_FORMAT_DIFF_ROOT}")
execute_process(
  COMMAND "${LIQUID_FORMAT_DIFF_FIXTURE}"
          "${LIQUID_FORMAT_DIFF_ROOT}"
          "${LIQUID_FORMAT_FIXTURES}"
  RESULT_VARIABLE fixture_result
  ERROR_VARIABLE fixture_error)
if(NOT fixture_result EQUAL 0)
  message(FATAL_ERROR "format diff fixture failed: ${fixture_error}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" diff before after --formats
  WORKING_DIRECTORY "${LIQUID_FORMAT_DIFF_ROOT}"
  RESULT_VARIABLE diff_result
  OUTPUT_VARIABLE diff_output
  ERROR_VARIABLE diff_error)
if(NOT diff_result EQUAL 0)
  message(FATAL_ERROR "liquid diff failed: ${diff_error}")
endif()
if(NOT diff_output MATCHES "archive\\.zip zip tier=2")
  message(FATAL_ERROR "zip container line is absent: ${diff_output}")
endif()
if(NOT diff_output MATCHES "modified deflated\\.txt")
  message(FATAL_ERROR "zip member line is absent: ${diff_output}")
endif()
if(NOT diff_output MATCHES "measurements\\.h5 hdf5 tier=2")
  message(FATAL_ERROR "HDF5 container line is absent: ${diff_output}")
endif()
if(NOT diff_output MATCHES "modified measurements/chunked")
  message(FATAL_ERROR "HDF5 dataset line is absent: ${diff_output}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" diff before after archive.zip --formats --json
  WORKING_DIRECTORY "${LIQUID_FORMAT_DIFF_ROOT}"
  RESULT_VARIABLE json_result
  OUTPUT_VARIABLE json_output
  ERROR_VARIABLE json_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT json_result EQUAL 0)
  message(FATAL_ERROR "liquid diff JSON failed: ${json_error}")
endif()
string(JSON json_entries LENGTH "${json_output}")
string(JSON json_path GET "${json_output}" 0 path)
string(JSON json_codec GET "${json_output}" 0 container codec)
if(NOT json_entries EQUAL 1 OR NOT json_path STREQUAL "archive.zip" OR
   NOT json_codec STREQUAL "zip")
  message(FATAL_ERROR "liquid diff JSON changed: ${json_output}")
endif()
