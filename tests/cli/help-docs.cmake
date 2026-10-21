# SPDX-License-Identifier: MIT

# Run with cmake -P: set the project's policies, so IN_LIST is an operator on CMake 3.x too (#364).
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED LIQUID_CLI)
  message(FATAL_ERROR "LIQUID_CLI is required")
endif()
if(NOT DEFINED LIQUID_CLI_DOCS)
  message(FATAL_ERROR "LIQUID_CLI_DOCS is required")
endif()
if(NOT DEFINED LIQUID_CLI_REFERENCE)
  message(FATAL_ERROR "LIQUID_CLI_REFERENCE is required")
endif()

execute_process(
  COMMAND "${LIQUID_CLI}" --help
  RESULT_VARIABLE help_result
  OUTPUT_VARIABLE help_output
  ERROR_VARIABLE help_error)
if(NOT help_result EQUAL 0)
  message(FATAL_ERROR "liquid --help failed: ${help_error}")
endif()

file(READ "${LIQUID_CLI_DOCS}" documentation)
string(REPLACE "\n" ";" help_lines "${help_output}")
set(help_commands)
foreach(line IN LISTS help_lines)
  if(NOT line MATCHES "^  ([a-z][a-z0-9_-]*) +")
    continue()
  endif()
  set(command "${CMAKE_MATCH_1}")
  list(APPEND help_commands "${command}")
  string(FIND "${documentation}" "](${command}.md)" documentation_position)
  if(documentation_position EQUAL -1)
    message(FATAL_ERROR "documentation is missing ${command}")
  endif()
  execute_process(
    COMMAND "${LIQUID_CLI}" "${command}" --help
    RESULT_VARIABLE command_help_result
    OUTPUT_VARIABLE command_help
    ERROR_VARIABLE command_help_error)
  if(NOT command_help_result EQUAL 0)
    message(FATAL_ERROR "liquid ${command} --help failed: ${command_help_error}")
  endif()
  set(reference "${LIQUID_CLI_REFERENCE}/${command}.md")
  if(NOT EXISTS "${reference}")
    message(FATAL_ERROR "CLI reference is missing ${command}.md")
  endif()
  file(READ "${reference}" command_documentation)
  string(REPLACE "\n" ";" command_help_lines "${command_help}")
  foreach(help_line IN LISTS command_help_lines)
    if(NOT help_line MATCHES "^  (--[a-z][a-z0-9-]*)( |$)")
      continue()
    endif()
    set(help_flag "${CMAKE_MATCH_1}")
    string(FIND "${command_documentation}" "| `${help_flag}` |" flag_position)
    if(flag_position EQUAL -1)
      message(FATAL_ERROR "${command} documentation is missing ${help_flag}")
    endif()
  endforeach()
  string(REGEX MATCHALL "\\| `--[a-z][a-z0-9-]*` \\|" documented_flags
    "${command_documentation}")
  foreach(documented_flag IN LISTS documented_flags)
    string(REGEX REPLACE "^\\| `([^`]+)` \\|$" "\\1" documented_flag "${documented_flag}")
    string(FIND "${command_help}" "  ${documented_flag}" flag_position)
    if(flag_position EQUAL -1)
      message(FATAL_ERROR "${command} --help is missing documented flag ${documented_flag}")
    endif()
  endforeach()
  execute_process(
    COMMAND "${LIQUID_CLI}" "${command}" --not-a-liquid-option
    RESULT_VARIABLE unknown_result
    OUTPUT_QUIET
    ERROR_QUIET)
  if(NOT unknown_result EQUAL 2)
    message(FATAL_ERROR "${command} accepted an option absent from its typed table")
  endif()
endforeach()

file(GLOB documented_pages RELATIVE "${LIQUID_CLI_REFERENCE}" "${LIQUID_CLI_REFERENCE}/*.md")
foreach(page IN LISTS documented_pages)
  if(page STREQUAL "README.md")
    continue()
  endif()
  string(REGEX REPLACE "\\.md$" "" command "${page}")
  if(NOT command IN_LIST help_commands)
    message(FATAL_ERROR "--help is missing documented command ${command}")
  endif()
endforeach()

# The help is grouped, and each command is listed in exactly one group.
foreach(title IN ITEMS
    "Start here:"
    "Record and inspect history:"
    "Compose repositories and resolve conflicts:"
    "Share and interchange:"
    "Capture, working trees and storage:"
    "Identity, keys and trust:"
    "Build:"
    "Package, install and deploy:"
    "About liquid:")
  string(FIND "${help_output}" "\n${title}\n" title_position)
  if(title_position EQUAL -1)
    message(FATAL_ERROR "--help is missing the section '${title}'")
  endif()
endforeach()
set(seen_commands)
foreach(command IN LISTS help_commands)
  if(command IN_LIST seen_commands)
    message(FATAL_ERROR "--help lists ${command} in more than one group")
  endif()
  list(APPEND seen_commands "${command}")
endforeach()

# Git's name is the command; the older spelling is still accepted.
if(NOT "mv" IN_LIST help_commands)
  message(FATAL_ERROR "--help is missing mv")
endif()
if("move" IN_LIST help_commands)
  message(FATAL_ERROR "--help lists move; the command is mv")
endif()
execute_process(
  COMMAND "${LIQUID_CLI}" move --help
  RESULT_VARIABLE move_result
  OUTPUT_VARIABLE move_help
  ERROR_QUIET)
if(NOT move_result EQUAL 0 OR NOT move_help MATCHES "usage: liquid mv <from> <to>")
  message(FATAL_ERROR "liquid move is no longer accepted as a spelling of mv")
endif()

# A Git command liquid does not have says what to use instead.
foreach(pair IN ITEMS "commit|liquid cut" "add|liquid cut" "grep|liquid search" "reflog|liquid ops")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 git_command)
  list(GET pair 1 expected)
  execute_process(
    COMMAND "${LIQUID_CLI}" "${git_command}"
    RESULT_VARIABLE hint_result
    OUTPUT_QUIET
    ERROR_VARIABLE hint_error)
  if(NOT hint_result EQUAL 2)
    message(FATAL_ERROR "liquid ${git_command} did not exit 2")
  endif()
  string(FIND "${hint_error}" "${expected}" hint_position)
  if(hint_position EQUAL -1)
    message(FATAL_ERROR "liquid ${git_command} does not point at ${expected}: ${hint_error}")
  endif()
endforeach()
