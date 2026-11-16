# Every CMake script under tests/ that uses IN_LIST sets its policies itself (liquid#364).
#
# Scripts run with `cmake -P` get no policies from the project. Under CMake 3.x, IN_LIST is
# not an if() operator until CMP0057 is NEW, so such a script fails there and passes on
# CMake 4, which defaults the policy to NEW. The check reads the text, so it fails on any
# CMake version.
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED LIQUID_TESTS_ROOT)
  message(FATAL_ERROR "LIQUID_TESTS_ROOT is required")
endif()
file(GLOB_RECURSE scripts RELATIVE "${LIQUID_TESTS_ROOT}" "${LIQUID_TESTS_ROOT}/*.cmake")
set(missing)
set(checked 0)
foreach(script IN LISTS scripts)
  file(READ "${LIQUID_TESTS_ROOT}/${script}" content)
  string(FIND "${content}" "IN_LIST" uses_in_list)
  if(uses_in_list EQUAL -1)
    continue()
  endif()
  math(EXPR checked "${checked} + 1")
  if(NOT content MATCHES "cmake_minimum_required\\(VERSION" AND
     NOT content MATCHES "cmake_policy\\(SET CMP0057 NEW\\)")
    list(APPEND missing "${script}")
  endif()
endforeach()
if(missing)
  list(JOIN missing "\n  " missing_text)
  message(FATAL_ERROR
    "these scripts use IN_LIST without setting CMP0057 (add cmake_minimum_required):\n"
    "  ${missing_text}")
endif()
message(STATUS "${checked} script(s) using IN_LIST set their policies")
