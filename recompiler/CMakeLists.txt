# ps1xRecomp — PS1 Static Recompiler

# Library target (for linking into tests)
add_library(ps1xRecomp_lib
    src/mips_decoder.cpp
    src/instruction_emitter.cpp
)

target_include_directories(ps1xRecomp_lib
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1xRecomp_lib
    PUBLIC
        fmt::fmt
)

# Executable target
add_executable(ps1xRecomp
    src/main.cpp
)

target_link_libraries(ps1xRecomp
    PRIVATE
        ps1xRecomp_lib
        toml11::toml11
)
