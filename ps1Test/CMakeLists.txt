# PS1Recomp Tests
# Structure: tests/<component>/test_<module>.cpp

# ──────────────────────────────────────────
# Analyzer Tests
# ──────────────────────────────────────────
add_executable(ps1Analyzer_tests
    analyzer/test_elf_parser.cpp
    analyzer/test_function_finder.cpp
    analyzer/test_psyq_signatures.cpp
    analyzer/test_psyq_hash_matching.cpp
    analyzer/test_config_generator.cpp
    analyzer/test_overlay_scanner.cpp
)

target_link_libraries(ps1Analyzer_tests
    PRIVATE
        GTest::gtest_main
        ps1Analyzer_lib
)

# Tests need the data dir (TOML signature DB) at runtime.
target_compile_definitions(ps1Analyzer_tests
    PRIVATE
        PS1RECOMP_DATA_DIR_DEFAULT="${CMAKE_SOURCE_DIR}/ps1Analyzer/data"
)

gtest_discover_tests(ps1Analyzer_tests)

# ──────────────────────────────────────────
# Recompiler Tests
# ──────────────────────────────────────────
add_executable(ps1Recomp_tests
    recompiler/test_mips_decoder.cpp
    recompiler/test_instruction_emitter.cpp
    recompiler/test_gte_emitter.cpp
    recompiler/test_overlay_handler.cpp
    recompiler/test_hle_emission.cpp
)

target_link_libraries(ps1Recomp_tests
    PRIVATE
        GTest::gtest_main
        ps1Recomp_lib
)

gtest_discover_tests(ps1Recomp_tests)

# ──────────────────────────────────────────
# Runtime Tests
# ──────────────────────────────────────────
add_executable(ps1Runtime_tests
    runtime/test_stubs.cpp
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
    runtime/test_gpu_texture_window.cpp
    runtime/test_gpu_blend.cpp
    runtime/test_gpu_display.cpp
    runtime/test_gpu_vram.cpp
    runtime/test_spu.cpp
    runtime/test_cdrom_controller.cpp
    runtime/test_dma.cpp
    runtime/test_input.cpp
    runtime/test_mdec.cpp
    runtime/test_timers.cpp
    runtime/test_save_state.cpp
    runtime/test_config.cpp
    runtime/test_gpu_screenshot.cpp
    runtime/test_game_validation.cpp
    runtime/test_overlay_manager.cpp
    runtime/test_psyq_hle.cpp
    runtime/test_psyq_registry.cpp
    runtime/test_psyq_libapi.cpp
    runtime/test_psyq_gpu.cpp
    runtime/test_psyq_cd.cpp
    runtime/test_psyq_gte.cpp
    runtime/test_psyq_font.cpp
    runtime/test_psyq_pad.cpp
    runtime/test_psyq_libc.cpp
)

target_include_directories(ps1Runtime_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/ps1Runtime/include
        ${CMAKE_SOURCE_DIR}/ps1Runtime/src
)

target_link_libraries(ps1Runtime_tests
    PRIVATE
        ps1Runtime_lib
        GTest::gtest_main
        fmt::fmt
)

gtest_discover_tests(ps1Runtime_tests)

# Phase 5 E2E Tests
add_executable(ps1Runtime_e2e_tests
    pipeline/test_pipeline.cpp
    pipeline/test_elf_loader.cpp
    pipeline/test_function_resolver.cpp
)
target_link_libraries(ps1Runtime_e2e_tests PRIVATE ps1Runtime_lib ps1Analyzer_lib ps1Recomp_lib gtest gtest_main gmock)
gtest_discover_tests(ps1Runtime_e2e_tests)
