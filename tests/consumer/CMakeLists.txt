cmake_minimum_required(VERSION 3.28)

project(liquid-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(liquid CONFIG REQUIRED)

add_executable(liquid-consumer main.cpp)
target_link_libraries(liquid-consumer PRIVATE liquid::liquid liquid::hot)
