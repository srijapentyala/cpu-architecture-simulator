cmake_minimum_required(VERSION 3.16)
project(cpu_architecture_simulator LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(SIM_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined")
if(SIM_SANITIZE)
  add_compile_options(-fsanitize=${SIM_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${SIM_SANITIZE})
endif()

include(FetchContent)
FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip)
set(DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_library(simlib src/isa.cpp src/memory.cpp src/predictor.cpp src/simulator.cpp)
target_include_directories(simlib PUBLIC include)
target_compile_options(simlib PRIVATE -Wall -Wextra)

add_executable(sim src/main.cpp)
target_link_libraries(sim PRIVATE simlib)

enable_testing()
add_executable(sim_tests tests/test_sim.cpp)
target_link_libraries(sim_tests PRIVATE simlib GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(sim_tests)

# Integration tests drive the built binary. They need pytest, so they register only when
# an interpreter that has it is available (see the README for the venv).
set(SIM_PYTHON "${CMAKE_SOURCE_DIR}/.venv/bin/python" CACHE FILEPATH "Python with pytest")
if(EXISTS "${SIM_PYTHON}")
  add_test(NAME pytest_integration
           COMMAND "${SIM_PYTHON}" -m pytest -q "${CMAKE_SOURCE_DIR}/tests/python"
           WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(pytest_integration PROPERTIES ENVIRONMENT "SIM_BINARY=$<TARGET_FILE:sim>")
endif()
