file(READ "${LIQUID_RETAIN_REPOSITORY}/RETAINED_DIGEST" retained_digest)
string(STRIP "${retained_digest}" retained_digest)

execute_process(
  COMMAND "${LIQUID_CLI}" retain explain "${retained_digest}"
  WORKING_DIRECTORY "${LIQUID_RETAIN_REPOSITORY}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "retain explain failed (${result})\nstdout:\n${output}\nstderr:\n${error}")
endif()
if(NOT output MATCHES "retained: lease [0-9a-f]+ from rule default refs")
  message(FATAL_ERROR "retain explain did not name the rule and lease:\n${output}")
endif()
if(NOT output MATCHES "until [0-9]+ for root")
  message(FATAL_ERROR "retain explain did not name the rule, lease, and expiry:\n${output}")
endif()
