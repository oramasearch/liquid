cmake_minimum_required(VERSION 3.28)

# liquid is C++26. Capy/Corosio (when they arrive) need Clang 17+, so prefer a
# modern Clang before the project() call — the same auto-selection Zetta uses.
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT LIQUID_USE_SYSTEM_COMPILER)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"
     AND EXISTS "/opt/homebrew/opt/llvm/bin/clang++")
    set(CMAKE_C_COMPILER   "/opt/homebrew/opt/llvm/bin/clang"   CACHE FILEPATH "")
    set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++" CACHE FILEPATH "")
    if(NOT CMAKE_OSX_SYSROOT)
      execute_process(COMMAND xcrun --show-sdk-path
                      OUTPUT_VARIABLE LIQUID_SDKROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
      set(CMAKE_OSX_SYSROOT "${LIQUID_SDKROOT}" CACHE PATH "")
    endif()
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    foreach(LIQUID_VER 22 21 20 19 18 17)
      find_program(LIQUID_CLANGXX_${LIQUID_VER} "clang++-${LIQUID_VER}")
      if(LIQUID_CLANGXX_${LIQUID_VER})
        set(CMAKE_CXX_COMPILER "${LIQUID_CLANGXX_${LIQUID_VER}}" CACHE FILEPATH "")
        find_program(LIQUID_CLANGC_${LIQUID_VER} "clang-${LIQUID_VER}")
        if(LIQUID_CLANGC_${LIQUID_VER})
          set(CMAKE_C_COMPILER "${LIQUID_CLANGC_${LIQUID_VER}}" CACHE FILEPATH "")
        endif()
        break()
      endif()
    endforeach()
  endif()
endif()

project(liquid VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LIQUID_BUILD_TESTS "Build the test suite" ON)

# Core library: everything the CLI and future daemon share.
add_library(liquid-core STATIC
  src/version.cpp
)
target_include_directories(liquid-core PUBLIC include)
target_compile_features(liquid-core PUBLIC cxx_std_26)

# The CLI.
add_executable(liquid src/main.cpp)
target_link_libraries(liquid PRIVATE liquid-core)

# Tests: GoogleTest via FetchContent + CTest, matching Zetta.
if(LIQUID_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_subdirectory(tests)
endif()
