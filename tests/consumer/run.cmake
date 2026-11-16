# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_BINARY_DIR OR NOT DEFINED LIQUID_CONSUMER_SOURCE_DIR OR
   NOT DEFINED LIQUID_CONSUMER_BINARY_DIR OR NOT DEFINED LIQUID_CONSUMER_INSTALL_DIR OR
   NOT DEFINED LIQUID_CONSUMER_GENERATOR OR NOT DEFINED LIQUID_CONSUMER_CXX_COMPILER)
  message(FATAL_ERROR "consumer test configuration is incomplete")
endif()

file(REMOVE_RECURSE "${LIQUID_CONSUMER_BINARY_DIR}")
file(REMOVE_RECURSE "${LIQUID_CONSUMER_INSTALL_DIR}")
set(relocated_install_dir "${LIQUID_CONSUMER_INSTALL_DIR}-relocated")
file(REMOVE_RECURSE "${relocated_install_dir}")

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
if(LIQUID_CONSUMER_HAS_CLI)
  foreach(page IN ITEMS liquid.1 liquid-docs.1 liquid-wrap.1)
    if(NOT EXISTS "${LIQUID_CONSUMER_INSTALL_DIR}/share/man/man1/${page}")
      message(FATAL_ERROR "consumer install is missing share/man/man1/${page}")
    endif()
  endforeach()
endif()

# Move the prefix before importing it and reject references to the source/build trees.
# System dependency paths (OpenSSL, SDKs, and platform libraries) remain valid.
file(RENAME "${LIQUID_CONSUMER_INSTALL_DIR}" "${relocated_install_dir}")
get_filename_component(liquid_source_dir "${LIQUID_CONSUMER_SOURCE_DIR}/../.." ABSOLUTE)
file(GLOB_RECURSE package_files "${relocated_install_dir}/*.cmake")
foreach(package_file IN LISTS package_files)
  file(READ "${package_file}" package_contents)
  foreach(forbidden_path IN ITEMS
      "${LIQUID_BINARY_DIR}" "${liquid_source_dir}" "${LIQUID_CONSUMER_INSTALL_DIR}")
    string(FIND "${package_contents}" "${forbidden_path}" path_position)
    if(NOT path_position EQUAL -1)
      message(FATAL_ERROR "installed package contains ${forbidden_path}: ${package_file}")
    endif()
  endforeach()
endforeach()

set(configure_command
  "${CMAKE_COMMAND}"
  -S "${LIQUID_CONSUMER_SOURCE_DIR}"
  -B "${LIQUID_CONSUMER_BINARY_DIR}"
  -G "${LIQUID_CONSUMER_GENERATOR}"
  "-DCMAKE_CXX_COMPILER=${LIQUID_CONSUMER_CXX_COMPILER}"
  "-DCMAKE_PREFIX_PATH=${relocated_install_dir}")
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
  COMMAND "${CMAKE_COMMAND}" --build "${LIQUID_CONSUMER_BINARY_DIR}"
    --parallel 6 --target liquid-consumer liquid-forge-double-consumer liquid-hot-consumer
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "consumer build failed\n${build_output}\n${build_error}")
endif()
message(STATUS "consumer build output:\n${build_output}")

file(MAKE_DIRECTORY "${LIQUID_CONSUMER_BINARY_DIR}/repository")
execute_process(
  COMMAND "${LIQUID_CONSUMER_BINARY_DIR}/liquid-consumer"
  WORKING_DIRECTORY "${LIQUID_CONSUMER_BINARY_DIR}/repository"
  RESULT_VARIABLE run_result)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "consumer executable failed")
endif()

execute_process(
  COMMAND "${LIQUID_CONSUMER_BINARY_DIR}/liquid-hot-consumer"
  RESULT_VARIABLE hot_run_result)
if(NOT hot_run_result EQUAL 0)
  message(FATAL_ERROR "hot ABI consumer executable failed")
endif()

execute_process(
  COMMAND "${LIQUID_CONSUMER_BINARY_DIR}/liquid-forge-double-consumer"
  RESULT_VARIABLE forge_run_result)
if(NOT forge_run_result EQUAL 0)
  message(FATAL_ERROR "forge double consumer executable failed")
endif()
