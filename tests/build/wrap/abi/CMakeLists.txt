cmake_minimum_required(VERSION 3.16)
project(liquid_wrap_abi C)

add_library(fixture SHARED abi.c)
target_compile_options(fixture PRIVATE -g)
target_link_options(fixture PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/abi.map")
