find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)

# ── Runtime Library (shared between exe and tests) ──────
add_library(ps1xRuntime_lib STATIC
    src/gte.cpp
)

target_include_directories(ps1xRuntime_lib
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1xRuntime_lib
    PUBLIC fmt::fmt
)

# ── Generate Recompiled Output (Phase 5 E2E Pipeline) ────
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled_out.cpp
    COMMAND ps1xAnalyzer ${CMAKE_SOURCE_DIR}/test_roms/example08_spinningCube.elf config.toml
    COMMAND ps1xRecomp config.toml ${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled_out.cpp
    DEPENDS ps1xAnalyzer ps1xRecomp ${CMAKE_SOURCE_DIR}/test_roms/example08_spinningCube.elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Auto-generating recompiled_out.cpp via ps1xAnalyzer and ps1xRecomp"
)

# ── Runtime Executable ──────────────────────────────────
add_executable(ps1xRuntime
    src/main_host.cpp
    src/recompiled_out.cpp
)

target_include_directories(ps1xRuntime
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1xRuntime
    PRIVATE
        ps1xRuntime_lib
        fmt::fmt
        SDL2::SDL2
        OpenGL::GL
        ps1xAnalyzer_lib
)
