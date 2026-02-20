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
