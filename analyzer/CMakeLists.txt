add_executable(ps1xAnalyzer
    src/main.cpp
)

target_include_directories(ps1xAnalyzer
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1xAnalyzer
    PRIVATE
        elfio
        toml11::toml11
        fmt::fmt
)
