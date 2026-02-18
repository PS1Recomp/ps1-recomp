# PS1Recomp Tests
# Structure: tests/<component>/test_<module>.cpp

# ──────────────────────────────────────────
# Analyzer Tests
# ──────────────────────────────────────────
add_executable(ps1xAnalyzer_tests
    analyzer/test_elf_parser.cpp
    analyzer/test_function_finder.cpp
    analyzer/test_psyq_signatures.cpp
    analyzer/test_config_generator.cpp
)

target_link_libraries(ps1xAnalyzer_tests
    PRIVATE
        GTest::gtest_main
        ps1xAnalyzer_lib
)

gtest_discover_tests(ps1xAnalyzer_tests)

# ──────────────────────────────────────────
# Recompiler Tests
# ──────────────────────────────────────────
add_executable(ps1xRecomp_tests
    recompiler/test_mips_decoder.cpp
    recompiler/test_instruction_emitter.cpp
    recompiler/test_gte_emitter.cpp
    recompiler/test_overlay_handler.cpp
)

target_link_libraries(ps1xRecomp_tests
    PRIVATE
        GTest::gtest_main
        ps1xRecomp_lib
)

gtest_discover_tests(ps1xRecomp_tests)

# ──────────────────────────────────────────
# Runtime Tests
# ──────────────────────────────────────────
add_executable(ps1xRuntime_tests
    runtime/test_cpu_context.cpp
)

target_include_directories(ps1xRuntime_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/runtime/include
        ${CMAKE_SOURCE_DIR}/runtime/src
)

target_link_libraries(ps1xRuntime_tests
    PRIVATE
        GTest::gtest_main
        fmt::fmt
)

gtest_discover_tests(ps1xRuntime_tests)
