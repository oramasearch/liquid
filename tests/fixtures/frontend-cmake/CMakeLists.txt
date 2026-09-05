cmake_minimum_required(VERSION 3.16)
project(liquid_frontend_cmake CXX)

add_library(functions OBJECT functions.cpp)
target_compile_features(functions PRIVATE cxx_std_20)
