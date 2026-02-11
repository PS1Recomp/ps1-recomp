find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)

add_executable(ps1xRuntime
    src/main.cpp
)

target_include_directories(ps1xRuntime
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1xRuntime
    PRIVATE
        fmt::fmt
        SDL2::SDL2
        OpenGL::GL
)
