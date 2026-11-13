cmake_minimum_required(VERSION 3.21)
project(monkey_dust_game CXX)

# Layout after the migration: this file lives in the tools checkout (tools/game). MD_ROOT = the project root that is being built (main), MD_TOOLS_DIR = the tools checkout.
# MD_SHADER_OVERLAY_DIR = the extra shader directory of the private repo (the translated shaders); empty = tools alone (those features are off or fall back).
get_filename_component(MD_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MD_ROOT "${CMAKE_SOURCE_DIR}")
if(NOT MD_GLSLANG_INC)
    set(MD_GLSLANG_INC "${MD_ROOT}/third_party/glslang_src")   # main: the FetchContent source dir of the root CMake; standalone: set by cmake/standalone.cmake
endif()
set(MD_SHADER_OVERLAY_DIR "" CACHE PATH "extra shader sources compiled together with tools/shaders (later directories override)")
set(MD_SPIRV_OUT_DIR "${MD_ROOT}/shaders/spirv" CACHE PATH "where the SPIR-V is written (the HAL reads <cwd>/shaders/spirv in development; the release embeds it)")

file(GLOB_RECURSE GAME_SOURCES "src/*.cpp")
if(NOT MD_HLSL_RUNTIME)
    list(FILTER GAME_SOURCES EXCLUDE REGEX "kenshi_hlsl\\.cpp$")   # path 1 needs glslang (root option MD_HLSL_RUNTIME)
endif()

# the title "monkey_dust v<MAJOR.MINOR> · <dd.mm> · <git hash>[-dirty]" is generated at BUILD time (only a tiny .cpp, rewritten only when the text changes)
set(MD_BUILD_INFO_CPP ${CMAKE_BINARY_DIR}/generated/build_info.cpp)
add_custom_target(md_build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${MD_ROOT} -DOUT=${MD_BUILD_INFO_CPP} -DVER=${CMAKE_PROJECT_VERSION_MAJOR}.${CMAKE_PROJECT_VERSION_MINOR} -P ${MD_TOOLS_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${MD_BUILD_INFO_CPP}
    COMMENT "build info")
# #201 R2: the SPIR-V is compiled from shaders/ at build time (scripts/compile_shaders.sh, skips up-to-date files) and embedded in the binary;
# at run time the HAL reads shaders/spirv/ only when it exists (development) and falls back to the embedded copy (release)
set(MD_SHADER_DIRS ${MD_TOOLS_DIR}/shaders)
if(MD_SHADER_OVERLAY_DIR)
    list(APPEND MD_SHADER_DIRS ${MD_SHADER_OVERLAY_DIR})
endif()
set(MD_SHADER_SOURCES)
foreach(_d ${MD_SHADER_DIRS})
    file(GLOB _s CONFIGURE_DEPENDS ${_d}/*.vert ${_d}/*.frag ${_d}/*.comp ${_d}/*.glsl)
    list(APPEND MD_SHADER_SOURCES ${_s})
endforeach()
string(REPLACE ";" ":" MD_SHADER_SRC_LIST "${MD_SHADER_DIRS}")
set(MD_EMBEDDED_SPIRV_CPP ${CMAKE_BINARY_DIR}/generated/embedded_spirv.cpp)
add_custom_command(OUTPUT ${MD_EMBEDDED_SPIRV_CPP}
    COMMAND ${CMAKE_COMMAND} -E env MD_SHADER_SRC=${MD_SHADER_SRC_LIST} MD_SPIRV_OUT=${MD_SPIRV_OUT_DIR} bash ${MD_TOOLS_DIR}/scripts/compile_shaders.sh
    COMMAND python3 ${MD_TOOLS_DIR}/scripts/embed_spirv.py ${MD_SPIRV_OUT_DIR} ${MD_EMBEDDED_SPIRV_CPP}
    DEPENDS ${MD_SHADER_SOURCES} ${MD_TOOLS_DIR}/scripts/compile_shaders.sh ${MD_TOOLS_DIR}/scripts/embed_spirv.py
    WORKING_DIRECTORY ${MD_ROOT}
    COMMENT "SPIR-V: compile and embed")
# #201 R5: our own small data files travel inside the binary (md::DataPath writes them to ~/.cache/monkeydust on first use)
set(MD_EMBED_DATA_FILES game/data/render_quality_overrides.txt=${MD_TOOLS_DIR}/game/data/render_quality_overrides.txt engine/THIRD_PARTY.md=${MD_ROOT}/engine/THIRD_PARTY.md game/src/render/ground_live_glue.hlsl=${MD_TOOLS_DIR}/game/src/render/ground_live_glue.hlsl game/src/render/ground_live_gbuf.hlsl=${MD_TOOLS_DIR}/game/src/render/ground_live_gbuf.hlsl game/src/render/ground_live_bits.hlsl=${MD_TOOLS_DIR}/game/src/render/ground_live_bits.hlsl game/src/render/ground_live_decode.hlsl=${MD_TOOLS_DIR}/game/src/render/ground_live_decode.hlsl game/src/render/ground_live_light_vs.hlsl=${MD_TOOLS_DIR}/game/src/render/ground_live_light_vs.hlsl game/src/render/ground_live_fog.hlsl=${MD_TOOLS_DIR}/game/src/render/ground_live_fog.hlsl game/src/render/prop_tri_live.hlsl=${MD_TOOLS_DIR}/game/src/render/prop_tri_live.hlsl game/src/render/ground_live_hdr.hlsl=${MD_TOOLS_DIR}/game/src/render/ground_live_hdr.hlsl game/src/render/ground_live_cache.hlsl=${MD_TOOLS_DIR}/game/src/render/ground_live_cache.hlsl)   # key=file: our HLSL glue of the ground live pass (path 1)
set(MD_EMBEDDED_DATA_CPP ${CMAKE_BINARY_DIR}/generated/embedded_data.cpp)
set(_MD_EMBED_DEPS)
foreach(_kv ${MD_EMBED_DATA_FILES})
    string(REGEX REPLACE "^[^=]*=" "" _f "${_kv}")
    list(APPEND _MD_EMBED_DEPS ${_f})
endforeach()
add_custom_command(OUTPUT ${MD_EMBEDDED_DATA_CPP}
    COMMAND python3 ${MD_TOOLS_DIR}/scripts/embed_data.py ${MD_EMBEDDED_DATA_CPP} ${MD_EMBED_DATA_FILES}
    DEPENDS ${_MD_EMBED_DEPS} ${MD_TOOLS_DIR}/scripts/embed_data.py
    COMMENT "embed data files")
add_executable(monkey_dust ${GAME_SOURCES} ${IMGUI_SOURCES} ${MD_BUILD_INFO_CPP} ${MD_EMBEDDED_SPIRV_CPP} ${MD_EMBEDDED_DATA_CPP})
set_source_files_properties(${MD_EMBEDDED_DATA_CPP} PROPERTIES OBJECT_DEPENDS "${_MD_EMBED_DEPS}")
if(MD_HLSL_RUNTIME)
    target_include_directories(monkey_dust PRIVATE ${MD_GLSLANG_INC})
    target_link_libraries(monkey_dust PRIVATE glslang SPIRV glslang-default-resource-limits SPIRV-Tools-opt SPIRV-Tools)
    target_compile_definitions(monkey_dust PRIVATE MD_HLSL_RUNTIME=1)
endif()
file(GLOB MD_SPV_FILES CONFIGURE_DEPENDS ${MD_SPIRV_OUT_DIR}/*.spv)
set_source_files_properties(${MD_EMBEDDED_SPIRV_CPP} PROPERTIES OBJECT_DEPENDS "${MD_SPV_FILES}")
add_dependencies(monkey_dust md_build_info)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/render/first_run_font.cpp PROPERTIES COMPILE_DEFINITIONS "MD_FONT_DIR=\"${MD_FONT_DIR}\"" OBJECT_DEPENDS "${MD_FONT_DIR}/Arimo-Regular.ttf")

if(MD_STATIC_DEPS)
    set(_GAME_LUA lua54_static)
    set(_GAME_LUA_INC "${MD_ROOT}/third_party/lua54_src/src")
else()
    set(_GAME_LUA ${LUA_LIBRARIES})
    set(_GAME_LUA_INC "${LUA_INCLUDE_DIR}")
endif()

if(MD_TRACY)
    target_link_libraries(monkey_dust PRIVATE TracyClient)
endif()

target_link_libraries(monkey_dust PRIVATE
    monkey_dust::engine
    ${_GAME_LUA}
)

target_include_directories(monkey_dust PRIVATE
    src/
    ${_GAME_LUA_INC}
)

target_include_directories(monkey_dust PRIVATE
    ${MD_TOOLS_DIR}/third_party/imgui
    ${MD_TOOLS_DIR}/third_party/imgui/backends)
# Raylib is no longer needed at link time.
target_sources(monkey_dust PRIVATE
    ${MD_ROOT}/engine/src/vendor/stb_image_impl.cpp
    ${MD_ROOT}/engine/src/render/cgltf_impl.cpp)
if(MD_STATIC_DEPS)
    target_link_libraries(monkey_dust PRIVATE SDL3::SDL3-static)
    target_link_options(monkey_dust PRIVATE -static-libstdc++ -static-libgcc)   # #201 R5: only glibc, the Vulkan loader and the window / audio stack stay dynamic
else()
    target_link_libraries(monkey_dust PRIVATE SDL3::SDL3)
endif()

if(USE_GLM)
    target_compile_definitions(monkey_dust PRIVATE USE_GLM)
endif()


if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(monkey_dust PRIVATE DEBUG)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # -ffast-math: RE confirms 17K-18K float chain ops in Kenshi+AI.exe = SIMD candidates.
    # -march=${MD_MARCH} (default native; CI: skylake, the laptop's family): AVX2 auto-vectorisation of float4 loops. A native build on the CI runner emitted AVX-512 (SIGILL on the laptop).
    if(NOT MD_MARCH)
        set(MD_MARCH native)
    endif()
    target_compile_options(monkey_dust PRIVATE -O2 -march=${MD_MARCH} -ffast-math)
endif()

if(MONKEY_DUST_EDITOR)
    # In-game F3 editor: the ImGui sources, its extensions and the decoupled panels live in the tools repo and come as the libraries md_ui_third_party / md_game_panels
    # (built from source by tools/CMakeLists.txt or prebuilt by the tools CI, -DMD_TOOLS_PREBUILT; #189 B). Whole archive: the panels register through static initialisers.
    # The only game-coupled panel, game/src/editor/editor_inspector.cpp, is part of GAME_SOURCES.
    target_link_libraries(monkey_dust PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,md_game_panels>" md_ui_third_party)
    target_compile_definitions(monkey_dust PRIVATE MONKEY_DUST_EDITOR DEBUG)
    target_include_directories(monkey_dust PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/editor/)

    # ── UiDriver: live ImGui-widget lookup for the F3 overlay ────────────────
    # Reuses the Phase 5 vendored third_party/imgui_test_engine (tools/), same
    # MD_UI_TESTS flag as monkey_dust_editor's panel smoke tests — but here it
    # drives a live, interactive command-file loop (game/src/ui_driver/) instead
    # of a batch TestFunc run. See game/src/ui_driver/ui_driver.h's doc comment.
    if(MD_UI_TESTS)
        set(_GAME_ITE_DIR ${MD_TOOLS_DIR}/third_party/imgui_test_engine/imgui_test_engine)
        target_sources(monkey_dust PRIVATE
            ${_GAME_ITE_DIR}/imgui_te_context.cpp
            ${_GAME_ITE_DIR}/imgui_te_coroutine.cpp
            ${_GAME_ITE_DIR}/imgui_te_engine.cpp
            ${_GAME_ITE_DIR}/imgui_te_exporters.cpp
            ${_GAME_ITE_DIR}/imgui_te_perftool.cpp
            ${_GAME_ITE_DIR}/imgui_te_ui.cpp
            ${_GAME_ITE_DIR}/imgui_te_utils.cpp
            ${_GAME_ITE_DIR}/imgui_capture_tool.cpp
        )
        target_include_directories(monkey_dust PRIVATE
            ${MD_TOOLS_DIR}/third_party/imgui_test_engine
            ${_GAME_ITE_DIR}
            ${_GAME_ITE_DIR}/thirdparty
            ${_GAME_ITE_DIR}/thirdparty/Str
            ${_GAME_ITE_DIR}/thirdparty/stb
        )
        target_compile_definitions(monkey_dust PRIVATE
            MD_UI_TESTS
            IMGUI_ENABLE_TEST_ENGINE
            IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1
        )
        set_source_files_properties(
            ${_GAME_ITE_DIR}/imgui_te_context.cpp
            ${_GAME_ITE_DIR}/imgui_te_coroutine.cpp
            ${_GAME_ITE_DIR}/imgui_te_engine.cpp
            ${_GAME_ITE_DIR}/imgui_te_exporters.cpp
            ${_GAME_ITE_DIR}/imgui_te_perftool.cpp
            ${_GAME_ITE_DIR}/imgui_te_ui.cpp
            ${_GAME_ITE_DIR}/imgui_te_utils.cpp
            ${_GAME_ITE_DIR}/imgui_capture_tool.cpp
            PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
        message(STATUS "[monkey_dust] MD_UI_TESTS: ENABLED (live UiDriver for F3 overlay)")
    endif()
endif()

# terrain-perf-measure (2026-08-12): see MD_PERF_TEST_HOOKS option doc comment
# (top-level CMakeLists.txt) -- independent of MONKEY_DUST_EDITOR, no ImGui.
if(MD_PERF_TEST_HOOKS)
    target_compile_definitions(monkey_dust PRIVATE MD_PERF_TEST_HOOKS)
    message(STATUS "[monkey_dust] MD_PERF_TEST_HOOKS: ENABLED (live tmp_/game_cmd/ hooks, no ImGui)")
endif()

# ── PCH для game/src — main.cpp має 103 include'и, кожен зміна = повний preprocess
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
    target_precompile_headers(monkey_dust PRIVATE
        <cstdint> <cstdlib> <cstring> <cstdio> <cmath>
    )
    # stb / cgltf single-file implementations: no PCH (macro conflicts)
    set(_GAME_THIRDPARTY_CPP
        ${MD_ROOT}/engine/src/vendor/stb_image_impl.cpp
        ${MD_ROOT}/engine/src/render/cgltf_impl.cpp
    )
    set_source_files_properties(${_GAME_THIRDPARTY_CPP} PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON)
    if(MD_GAIA_PCH_HOT)
        md_gaia_pch(monkey_dust)
    endif()
    message(STATUS "[monkey_dust] game PCH: ENABLED")
endif()
