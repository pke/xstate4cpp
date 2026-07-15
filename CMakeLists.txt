cmake_minimum_required(VERSION 3.16)
project(xstate4cpp VERSION 0.1.0 LANGUAGES CXX)

add_library(xstate INTERFACE)
add_library(xstate::xstate ALIAS xstate)
target_include_directories(xstate INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(xstate INTERFACE cxx_std_17)

option(XSTATE_BUILD_TESTS "Build xstate tests" ON)
option(XSTATE_BUILD_EXAMPLES "Build xstate examples" ON)
if(XSTATE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(XSTATE_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt)
  add_subdirectory(examples)
endif()
