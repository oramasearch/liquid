foreach(required IN ITEMS LIQUID_CLI LIQUID_API_TESTS LIQUID_LOG_URL_WORK)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

if(DEFINED LIQUID_OFFLINE_RUNNER AND NOT LIQUID_LOG_URL_CHILD)
  execute_process(
    COMMAND "${LIQUID_OFFLINE_RUNNER}"
      "${LIQUID_LOG_URL_WORK}/sandbox" "${LIQUID_SOURCE_ROOT}" --
      "${CMAKE_COMMAND}"
      -DLIQUID_LOG_URL_CHILD=ON
      -DLIQUID_CLI=${LIQUID_CLI}
      -DLIQUID_API_TESTS=${LIQUID_API_TESTS}
      -DLIQUID_LOG_URL_WORK=${LIQUID_LOG_URL_WORK}
      -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE sandbox_result
    OUTPUT_VARIABLE sandbox_output
    ERROR_VARIABLE sandbox_error)
  if(NOT sandbox_result STREQUAL "0")
    message(FATAL_ERROR
      "log URL regression failed with networking denied (${sandbox_result}):\n${sandbox_output}${sandbox_error}")
  endif()
  message(STATUS "log URL regression with networking denied:\n${sandbox_output}${sandbox_error}")
  return()
endif()

file(MAKE_DIRECTORY "${LIQUID_LOG_URL_WORK}/no-repository")
execute_process(
  COMMAND "${LIQUID_API_TESTS}" --gtest_filter=HttpLogSource.*
  RESULT_VARIABLE api_result
  OUTPUT_VARIABLE api_output
  ERROR_VARIABLE api_error)
if(NOT api_result STREQUAL "0")
  message(FATAL_ERROR "HTTP log source API regression (${api_result}):\n${api_output}${api_error}")
endif()
message(STATUS "${api_output}")

function(require_invalid_url url diagnostic)
  foreach(verb IN ITEMS scan consistency verify trust)
    set(arguments log "${verb}" "${url}")
    if(verb STREQUAL "scan")
      list(APPEND arguments taap:1:ed25519:example)
    elseif(verb STREQUAL "verify")
      list(APPEND arguments AA)
    endif()
    execute_process(
      COMMAND "${LIQUID_CLI}" ${arguments}
      WORKING_DIRECTORY "${LIQUID_LOG_URL_WORK}/no-repository"
      RESULT_VARIABLE command_result
      OUTPUT_VARIABLE command_output
      ERROR_VARIABLE command_error)
    if(NOT command_result STREQUAL "2" OR command_output OR
       NOT command_error MATCHES "invalid_argument" OR
       NOT command_error MATCHES "HTTP" OR
       NOT command_error MATCHES "${diagnostic}")
      message(FATAL_ERROR
        "log ${verb} ${url}: exit=${command_result}\n${command_output}${command_error}")
    endif()
    string(STRIP "${command_error}" command_error)
    message(STATUS "log ${verb} ${url}: ${command_error}; exit=${command_result}")
  endforeach()
endfunction()

foreach(url IN ITEMS not-a-url ftp://127.0.0.1 http:/127.0.0.1 https//127.0.0.1)
  require_invalid_url("${url}" "http:// or https://")
endforeach()
foreach(url IN ITEMS http:// https:// http:///log https://:443/log
    http://[]:80/log http://?query "https://#fragment")
  require_invalid_url("${url}" "host")
endforeach()
foreach(url IN ITEMS http://127.0.0.1: https://[::1]: http://127.0.0.1:http
    https://127.0.0.1:443x http://127.0.0.1:0 https://127.0.0.1:65536
    http://127.0.0.1:-1 http://127.0.0.1:+80 http://127.0.0.1:999999999999999999999999)
  require_invalid_url("${url}" "port")
endforeach()
foreach(url IN ITEMS http://lake.example.invalid http://10.0.0.1:8080/log)
  require_invalid_url("${url}" "loopback")
endforeach()
