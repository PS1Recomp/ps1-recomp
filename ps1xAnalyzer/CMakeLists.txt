# ps1xAnalyzer — PS1 ELF Analyzer

# Library target (for linking into tests)
add_library(ps1xAnalyzer_lib
    src/elf_parser.cpp
    src/function_finder.cpp
    src/psyq_signatures.cpp
    src/config_generator.cpp
    src/disc_reader.cpp
    src/overlay_scanner.cpp
)

target_include_directories(ps1xAnalyzer_lib
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1xAnalyzer_lib
    PUBLIC
        elfio
        fmt::fmt
        toml11::toml11
)

# Executable target
add_executable(ps1xAnalyzer
    src/main.cpp
)

target_link_libraries(ps1xAnalyzer
    PRIVATE
        ps1xAnalyzer_lib
        toml11::toml11
)
