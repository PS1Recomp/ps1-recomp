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
    runtime/test_memory.cpp
    runtime/test_runtime_macros.cpp
    runtime/test_gte.cpp
    runtime/test_hw_stubs.cpp
    runtime/test_cue_parser.cpp
    runtime/test_bin_reader.cpp
    runtime/test_iso9660.cpp
    runtime/test_exe_extractor.cpp
    runtime/test_virtual_fs.cpp
    runtime/test_heap.cpp
    runtime/test_event_system.cpp
    runtime/test_file_io.cpp
    runtime/test_bios.cpp
    runtime/test_bios_table_a.cpp
    runtime/test_bios_table_b.cpp
    runtime/test_bios_table_c.cpp
    runtime/test_bios_integration.cpp
    runtime/test_gpu_commands.cpp
    runtime/test_gpu_dma.cpp
    runtime/test_gpu_lines_rects.cpp
    runtime/test_gpu_rasterizer.cpp
    runtime/test_gpu_textures.cpp
    runtime/test_gpu_vram.cpp
)

target_include_directories(ps1xRuntime_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/runtime/include
        ${CMAKE_SOURCE_DIR}/runtime/src
)

target_link_libraries(ps1xRuntime_tests
    PRIVATE
        ps1xRuntime_lib
        GTest::gtest_main
        fmt::fmt
)

gtest_discover_tests(ps1xRuntime_tests)

# Phase 5 E2E Tests
add_executable(ps1xRuntime_e2e_tests
    pipeline/test_pipeline.cpp
    pipeline/test_elf_loader.cpp
    pipeline/test_function_resolver.cpp
)
target_link_libraries(ps1xRuntime_e2e_tests PRIVATE ps1xRuntime_lib ps1xAnalyzer_lib ps1xRecomp_lib gtest gtest_main gmock)
gtest_discover_tests(ps1xRuntime_e2e_tests)
