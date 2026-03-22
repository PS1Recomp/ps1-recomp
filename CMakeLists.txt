cmake_minimum_required(VERSION 3.20)
project(PS1Recomp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Third-party libraries
add_subdirectory(third_party/ELFIO)
add_subdirectory(third_party/toml11)
add_subdirectory(third_party/fmt)

# Project components
add_subdirectory(ps1Analyzer)
add_subdirectory(ps1Recomp)
add_subdirectory(ps1Runtime)

# GUI Studio (optional, disabled by default — requires SDL2 + OpenGL)
option(PS1RECOMP_BUILD_INTERFACE "Build ps1Interface GUI studio" OFF)
if(PS1RECOMP_BUILD_INTERFACE)
    add_subdirectory(ps1Interface)
endif()

# Tests (optional, enabled by default)
option(PS1RECOMP_BUILD_TESTS "Build tests" ON)
if(PS1RECOMP_BUILD_TESTS)
    # GoogleTest — disable install to avoid polluting the project
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/googletest)
    include(GoogleTest)
    enable_testing()
    add_subdirectory(ps1Test)
endif()
