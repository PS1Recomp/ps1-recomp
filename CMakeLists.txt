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
add_subdirectory(ps1xAnalyzer)
add_subdirectory(ps1xRecomp)
add_subdirectory(ps1xRuntime)

# Tests (optional, enabled by default)
option(PS1RECOMP_BUILD_TESTS "Build tests" ON)
if(PS1RECOMP_BUILD_TESTS)
    # GoogleTest — disable install to avoid polluting the project
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(lib/googletest)
    include(GoogleTest)
    enable_testing()
    add_subdirectory(ps1xTest)
endif()
