# SPDX-License-Identifier: MIT

if(NOT DEFINED LIQUID_CLI)
  message(FATAL_ERROR "LIQUID_CLI is required")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" tiers --json
  RESULT_VARIABLE first_result
  OUTPUT_VARIABLE first_output
  ERROR_VARIABLE first_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
  COMMAND "${LIQUID_CLI}" tiers --json
  RESULT_VARIABLE second_result
  OUTPUT_VARIABLE second_output
  ERROR_VARIABLE second_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT first_result EQUAL 0 OR NOT second_result EQUAL 0)
  message(FATAL_ERROR "liquid tiers --json failed: ${first_error}${second_error}")
endif()
if(NOT first_output STREQUAL second_output)
  message(FATAL_ERROR "liquid tiers --json is unstable")
endif()

string(JSON language_count LENGTH "${first_output}" languages)
string(JSON format_count LENGTH "${first_output}" formats)
string(JSON platform_count LENGTH "${first_output}" platforms)
if(language_count LESS 2 OR format_count LESS 2 OR NOT platform_count EQUAL 4)
  message(FATAL_ERROR "liquid tiers --json omitted matrix rows: ${first_output}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" tiers --language c++ --json
  RESULT_VARIABLE cpp_result
  OUTPUT_VARIABLE cpp_output
  ERROR_VARIABLE cpp_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT cpp_result EQUAL 0)
  message(FATAL_ERROR "C++ tier filter failed: ${cpp_error}")
endif()
string(JSON cpp_tier GET "${cpp_output}" languages 0 tier)
string(JSON cpp_mechanism GET "${cpp_output}" languages 0 mechanism)
if(NOT cpp_tier EQUAL 3 OR NOT cpp_mechanism STREQUAL "frontend(clang-cpp)")
  message(FATAL_ERROR "C++ tier changed: ${cpp_output}")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" tiers --format zip --json
  RESULT_VARIABLE zip_result
  OUTPUT_VARIABLE zip_output
  ERROR_VARIABLE zip_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT zip_result EQUAL 0)
  message(FATAL_ERROR "ZIP tier filter failed: ${zip_error}")
endif()
string(JSON zip_codec GET "${zip_output}" formats 0 codec)
if(NOT zip_codec STREQUAL "zip")
  message(FATAL_ERROR "ZIP tier changed: ${zip_output}")
endif()
