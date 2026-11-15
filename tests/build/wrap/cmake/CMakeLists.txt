# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)
project(liquid_wrap_cmake C)

add_executable(wrapped main.c left.c right.c)
target_include_directories(wrapped PRIVATE include)

enable_testing()
add_test(NAME wrapped-smoke COMMAND wrapped)
set_tests_properties(wrapped-smoke PROPERTIES LABELS "quick;wrapped")
