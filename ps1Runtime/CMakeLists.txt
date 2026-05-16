find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)

# Runtime library (shared between exe and tests)
add_library(ps1Runtime_lib STATIC
    src/gte.cpp
    src/cdrom/cue_parser.cpp
    src/cdrom/bin_reader.cpp
    src/cdrom/iso9660.cpp
    src/cdrom/exe_extractor.cpp
    src/cdrom/virtual_fs.cpp
    src/cdrom/cdrom_controller.cpp
    src/bios/bios.cpp
    src/bios/syscall_a.cpp
    src/bios/syscall_b.cpp
    src/bios/syscall_c.cpp
    src/bios/heap.cpp
    src/bios/event_system.cpp
    src/bios/file_io.cpp
    src/gpu/gpu.cpp
    src/gpu/renderer_opengl.cpp
    src/spu/spu.cpp
    src/dma/dma.cpp
    src/input/input.cpp
    src/mdec/mdec.cpp
    src/timers/timers.cpp
    src/save_state.cpp
    src/config.cpp
    src/overlay_manager.cpp
    src/psyq/psyq_hle.cpp
    src/psyq/psyq_registry.cpp
    src/psyq/psyq_libapi.cpp
    src/psyq/psyq_libcd.cpp
    src/psyq/psyq_libetc.cpp
    src/psyq/psyq_libgte.cpp
    src/psyq/psyq_libgpu.cpp
    src/psyq/psyq_font.cpp
    src/psyq/psyq_pad.cpp
    src/psyq/psyq_libc.cpp
    src/psyq/psyq_state.cpp
)

target_include_directories(ps1Runtime_lib
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1Runtime_lib
    PUBLIC fmt::fmt
)

# Recompiled output generation (manual; see CONTRIBUTING.md)
# Auto-generation from spinningCube is disabled when using custom game configs.
# To regenerate manually:
#   cd build && ./ps1Recomp/ps1Recomp crash_config.toml ../ps1Runtime/src/recompiled_out.cpp
#
# add_custom_command(
#     OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled_out.cpp
#     COMMAND ps1Analyzer ${CMAKE_SOURCE_DIR}/test_roms/example08_spinningCube.elf config.toml
#     COMMAND ps1Recomp config.toml ${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled_out.cpp
#     DEPENDS ps1Analyzer ps1Recomp ${CMAKE_SOURCE_DIR}/test_roms/example08_spinningCube.elf
#     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
#     COMMENT "Auto-generating recompiled_out.cpp via ps1Analyzer and ps1Recomp"
# )

# Runtime executable
# PS1RECOMP_USE_STUB: force stub mode even when recompiled_out.cpp exists.
# Useful for CI pipelines and contributors who don't have a game ROM.
option(PS1RECOMP_USE_STUB "Use stub recompiled_out instead of real game" OFF)

if(PS1RECOMP_USE_STUB)
    set(RECOMP_SRC src/recompiled_out_stub.cpp)
    message(STATUS "PS1RECOMP_USE_STUB=ON — using stub (no ROM required)")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/recompiled_out.cpp")
    set(RECOMP_SRC src/recompiled_out.cpp)
else()
    set(RECOMP_SRC src/recompiled_out_stub.cpp)
    message(STATUS "recompiled_out.cpp not found — using stub (CI/test build)")
endif()

add_executable(ps1Runtime
    src/main_host.cpp
    ${RECOMP_SRC}
)

target_include_directories(ps1Runtime
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1Runtime
    PRIVATE
        ps1Runtime_lib
        fmt::fmt
        SDL2::SDL2
        OpenGL::GL
        ps1Analyzer_lib
)
