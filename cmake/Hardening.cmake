# SPDX-License-Identifier: MIT

# Hardening, sanitizer and fuzzing flags for every C and C++ target in the build (liquid #407).
#
# Included right after project() and before any dependency is added, so the options below reach
# libliquid, the CLI, the tests and every dependency compiled from source in this tree: Capy,
# Corosio, Boost.HTTP, zstd, BLAKE3, Monocypher, automerge-cpp and oramacore-cpp. Directory-scoped
# add_compile_options() is inherited by every subdirectory and FetchContent project added later.
#
#   LIQUID_HARDEN   ON by default. Stack protector, stack clash protection, control-flow
#                   protection where the target supports it, standard library assertions,
#                   position-independent executables and, on Linux, full RELRO and a
#                   non-executable stack. _FORTIFY_SOURCE=3 is added to optimised
#                   configurations (Release, RelWithDebInfo, MinSizeRel), the only ones where it
#                   has an effect, and is left out of sanitizer builds, where it hides findings.
#   LIQUID_SANITIZE A list of sanitizers, for example "address;undefined". Every C and C++ target
#                   is compiled and linked with them, with -fno-sanitize-recover=all, so the first
#                   finding stops the process with a non-zero exit.
#   LIQUID_FUZZ     Builds the libFuzzer harnesses under fuzz/. Implies
#                   LIQUID_SANITIZE=address;undefined when no sanitizer is named, and adds
#                   libFuzzer coverage instrumentation to every target. Requires
#                   Clang and a static libliquid, because the harnesses call internal decoders.
#
# Targets that must not depend on a sanitizer runtime set the target property
# LIQUID_NO_SANITIZER_RUNTIME (see liquid_no_sanitizer_runtime() below): the wrap hook, which is
# preloaded into uninstrumented build tools, and the statically linked programs, which the
# AddressSanitizer runtime does not support. They keep UndefinedBehaviorSanitizer in trap mode,
# which needs no runtime, and lose AddressSanitizer and coverage instrumentation.
#
# How to run each configuration, locally and on the Linux BuildKit lane: docs/build/hardening.md.

include(CheckCompilerFlag)
include(CheckLinkerFlag)

option(LIQUID_HARDEN "Compile with stack protection, fortified libc calls, PIE and full RELRO" ON)
set(LIQUID_SANITIZE "" CACHE STRING
    "Sanitizers for every C and C++ target, for example address;undefined")
option(LIQUID_FUZZ "Build the libFuzzer harnesses in fuzz/ (Clang; implies address;undefined)" OFF)

if(LIQUID_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LIQUID_FUZZ requires Clang: libFuzzer is part of compiler-rt.")
  endif()
  if(BUILD_SHARED_LIBS)
    message(FATAL_ERROR
      "LIQUID_FUZZ requires a static libliquid: the harnesses call decoders that a shared "
      "libliquid does not export. Configure with -DBUILD_SHARED_LIBS=OFF.")
  endif()
  if(NOT LIQUID_SANITIZE)
    set(LIQUID_SANITIZE "address;undefined")
  endif()
endif()

# The property is read through a generator expression, so it can be set after this file runs.
set(LIQUID_SANITIZER_RUNTIME_ALLOWED
    "$<NOT:$<BOOL:$<TARGET_PROPERTY:LIQUID_NO_SANITIZER_RUNTIME>>>")
set(LIQUID_C_OR_CXX "$<COMPILE_LANGUAGE:C,CXX>")

function(liquid_no_sanitizer_runtime)
  set_target_properties(${ARGN} PROPERTIES LIQUID_NO_SANITIZER_RUNTIME ON)
endfunction()

if(LIQUID_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "LIQUID_SANITIZE is implemented for Clang and GCC command lines only.")
  endif()
  set(LIQUID_SANITIZE_UNDEFINED OFF)
  foreach(LIQUID_SANITIZER IN LISTS LIQUID_SANITIZE)
    add_compile_options(
      "$<$<AND:${LIQUID_C_OR_CXX},${LIQUID_SANITIZER_RUNTIME_ALLOWED}>:-fsanitize=${LIQUID_SANITIZER}>")
    add_link_options(
      "$<${LIQUID_SANITIZER_RUNTIME_ALLOWED}:-fsanitize=${LIQUID_SANITIZER}>")
    if(LIQUID_SANITIZER STREQUAL "undefined")
      set(LIQUID_SANITIZE_UNDEFINED ON)
    endif()
  endforeach()
  add_compile_options(
    "$<${LIQUID_C_OR_CXX}:-fno-sanitize-recover=all>"
    "$<${LIQUID_C_OR_CXX}:-fno-omit-frame-pointer>"
    "$<${LIQUID_C_OR_CXX}:-g>")
  if(LIQUID_SANITIZE_UNDEFINED)
    # Undefined behaviour in a target without a runtime traps instead of printing a report.
    add_compile_options(
      "$<$<AND:${LIQUID_C_OR_CXX},$<NOT:${LIQUID_SANITIZER_RUNTIME_ALLOWED}>>:-fsanitize=undefined>"
      "$<$<AND:${LIQUID_C_OR_CXX},$<NOT:${LIQUID_SANITIZER_RUNTIME_ALLOWED}>>:-fsanitize-trap=undefined>")
    add_link_options(
      "$<$<NOT:${LIQUID_SANITIZER_RUNTIME_ALLOWED}>:-fsanitize=undefined>"
      "$<$<NOT:${LIQUID_SANITIZER_RUNTIME_ALLOWED}>:-fsanitize-trap=undefined>")
  endif()
  message(STATUS "liquid: sanitizers ${LIQUID_SANITIZE} on every C and C++ target")
endif()

if(LIQUID_FUZZ)
  # -fsanitize=fuzzer-no-link adds a PC table. Apple's ld (ld-1053, Xcode 15) refuses objects
  # that carry both that table and UndefinedBehaviorSanitizer data ("invalid r_symbolnum=1"), so
  # Darwin uses the coverage libFuzzer needs without the table: 8-bit edge counters and
  # comparison tracing. libFuzzer's coverage guidance is the same; only -print_pcs style
  # reporting is lost.
  if(APPLE)
    set(LIQUID_FUZZ_COVERAGE -fsanitize-coverage=inline-8bit-counters,trace-cmp)
  else()
    set(LIQUID_FUZZ_COVERAGE -fsanitize=fuzzer-no-link)
  endif()
  add_compile_options(
    "$<$<AND:${LIQUID_C_OR_CXX},${LIQUID_SANITIZER_RUNTIME_ALLOWED}>:${LIQUID_FUZZ_COVERAGE}>")
  # Lets a decoder relax a check that would stop the fuzzer from reaching deeper code, such as a
  # digest comparison. Nothing in the tree uses it yet; it is the libFuzzer convention.
  add_compile_definitions(LIQUID_FUZZING_BUILD=1)
  message(STATUS "liquid: libFuzzer coverage on every C and C++ target; harnesses in fuzz/")
endif()

if(LIQUID_HARDEN AND NOT MSVC)
  foreach(LIQUID_HARDEN_FLAG IN ITEMS
      -fstack-protector-strong
      -fstack-clash-protection
      -fcf-protection=full)
    string(MAKE_C_IDENTIFIER "LIQUID_HAS${LIQUID_HARDEN_FLAG}" LIQUID_HARDEN_CHECK)
    # -Werror turns "argument unused" into a failure, so a flag the target ignores (stack clash
    # protection on Darwin, control-flow protection off x86) is left out instead of warned on.
    check_compiler_flag(CXX "-Werror ${LIQUID_HARDEN_FLAG}" ${LIQUID_HARDEN_CHECK})
    if(${LIQUID_HARDEN_CHECK})
      add_compile_options("$<${LIQUID_C_OR_CXX}:${LIQUID_HARDEN_FLAG}>")
      list(APPEND LIQUID_HARDEN_APPLIED ${LIQUID_HARDEN_FLAG})
    endif()
  endforeach()

  # _FORTIFY_SOURCE needs optimisation; AddressSanitizer replaces the checks it would add.
  if(NOT LIQUID_SANITIZE)
    add_compile_options(
      "$<$<AND:${LIQUID_C_OR_CXX},$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:-U_FORTIFY_SOURCE>"
      "$<$<AND:${LIQUID_C_OR_CXX},$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:-D_FORTIFY_SOURCE=3>")
    list(APPEND LIQUID_HARDEN_APPLIED "-D_FORTIFY_SOURCE=3 (optimised configurations)")
  endif()

  # Bounds and precondition checks in the C++ standard library: libstdc++'s assertions and
  # libc++'s fast hardening mode. Each library ignores the other's macro.
  add_compile_definitions(
    "$<$<COMPILE_LANGUAGE:CXX>:_GLIBCXX_ASSERTIONS>"
    "$<$<COMPILE_LANGUAGE:CXX>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>")
  list(APPEND LIQUID_HARDEN_APPLIED _GLIBCXX_ASSERTIONS _LIBCPP_HARDENING_MODE_FAST)

  # Position-independent code everywhere, and position-independent executables at link time.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  include(CheckPIESupported)
  check_pie_supported(OUTPUT_VARIABLE LIQUID_PIE_OUTPUT LANGUAGES C CXX)
  if(CMAKE_C_LINK_PIE_SUPPORTED AND CMAKE_CXX_LINK_PIE_SUPPORTED)
    list(APPEND LIQUID_HARDEN_APPLIED PIE)
  elseif(NOT APPLE)
    message(WARNING "liquid: the linker cannot produce position-independent executables")
  else()
    # Apple's linker produces PIE for every arm64 and x86_64 executable unconditionally.
    list(APPEND LIQUID_HARDEN_APPLIED "PIE (platform default)")
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    check_linker_flag(C "LINKER:-z,relro,-z,now" LIQUID_HAS_FULL_RELRO)
    if(LIQUID_HAS_FULL_RELRO)
      add_link_options("LINKER:-z,relro,-z,now" "LINKER:-z,noexecstack")
      list(APPEND LIQUID_HARDEN_APPLIED "full RELRO" noexecstack)
    endif()
  endif()

  list(JOIN LIQUID_HARDEN_APPLIED ", " LIQUID_HARDEN_SUMMARY)
  message(STATUS "liquid: hardening ${LIQUID_HARDEN_SUMMARY}")
endif()
