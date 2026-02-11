cmake_minimum_required(VERSION 3.20)
project(PS1Recomp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Third-party libraries
add_subdirectory(lib/ELFIO)
add_subdirectory(lib/toml11)
add_subdirectory(lib/fmt)

# Project components
add_subdirectory(analyzer)
add_subdirectory(recompiler)
add_subdirectory(runtime)

# Tests (optional)
option(PS1RECOMP_BUILD_TESTS "Build tests" ON)
if(PS1RECOMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
