if(NOT DEFINED LIQUID_BINARY_DIR OR NOT DEFINED LIQUID_CONSUMER_SOURCE_DIR OR
   NOT DEFINED LIQUID_CONSUMER_BINARY_DIR OR NOT DEFINED LIQUID_CONSUMER_INSTALL_DIR OR
   NOT DEFINED LIQUID_CONSUMER_GENERATOR OR NOT DEFINED LIQUID_CONSUMER_CXX_COMPILER)
  message(FATAL_ERROR "consumer test configuration is incomplete")
endif()

file(REMOVE_RECURSE "${LIQUID_CONSUMER_BINARY_DIR}")
file(REMOVE_RECURSE "${LIQUID_CONSUMER_INSTALL_DIR}")

set(install_command
  "${CMAKE_COMMAND}"
  --install "${LIQUID_BINARY_DIR}"
  --prefix "${LIQUID_CONSUMER_INSTALL_DIR}")
if(DEFINED LIQUID_CONSUMER_BUILD_TYPE AND NOT LIQUID_CONSUMER_BUILD_TYPE STREQUAL "")
  list(APPEND install_command --config "${LIQUID_CONSUMER_BUILD_TYPE}")
endif()
execute_process(
  COMMAND ${install_command}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "consumer install failed\n${install_output}\n${install_error}")
endif()

set(configure_command
  "${CMAKE_COMMAND}"
  -S "${LIQUID_CONSUMER_SOURCE_DIR}"
  -B "${LIQUID_CONSUMER_BINARY_DIR}"
  -G "${LIQUID_CONSUMER_GENERATOR}"
  "-DCMAKE_CXX_COMPILER=${LIQUID_CONSUMER_CXX_COMPILER}"
  "-DCMAKE_PREFIX_PATH=${LIQUID_CONSUMER_INSTALL_DIR}")
if(DEFINED LIQUID_CONSUMER_BUILD_TYPE AND NOT LIQUID_CONSUMER_BUILD_TYPE STREQUAL "")
  list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${LIQUID_CONSUMER_BUILD_TYPE}")
endif()
execute_process(
  COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "consumer configure failed\n${configure_output}\n${configure_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${LIQUID_CONSUMER_BINARY_DIR}" --target liquid-consumer
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "consumer build failed\n${build_output}\n${build_error}")
endif()

execute_process(
  COMMAND "${LIQUID_CONSUMER_BINARY_DIR}/liquid-consumer"
  RESULT_VARIABLE run_result)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "consumer executable failed")
endif()
