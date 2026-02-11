add_executable(ps1xRecomp
    src/main.cpp
)

target_include_directories(ps1xRecomp
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1xRecomp
    PRIVATE
        toml11::toml11
        fmt::fmt
)
