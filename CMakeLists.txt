cmake_minimum_required(VERSION 3.21)
project(monkey_dust_tools CXX)

# SDL3 required for the editor.
option(MONKEY_DUST_TOOLS_NEED_SDL3 "Require SDL3 (editor)" ON)
if(MONKEY_DUST_TOOLS_NEED_SDL3)
    find_package(SDL3 REQUIRED CONFIG)
endif()

# ── md_character: character editor data library (no ImGui, no editor globals) ──
add_library(md_character STATIC
    editor/character/xml_mini.cpp editor/character/editor_limits.cpp editor/character/races.cpp editor/character/appearance.cpp editor/character/bod2.cpp editor/character/race_assets.cpp editor/character/bone_scales.cpp editor/character/skinned_body.cpp editor/character/pose_solver.cpp editor/character/attachments.cpp editor/character/appearance_io.cpp editor/character/race_stats.cpp editor/character/face_poses.cpp editor/character/session.cpp)
target_include_directories(md_character PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/editor/character)
target_link_libraries(md_character PUBLIC monkey_dust::engine)

# CI test of the editor logic on a synthetic install (no Kenshi files): ninja md_session_synthetic_test && ctest -R session_synthetic
add_executable(md_session_synthetic_test tests/session_synthetic_test.cpp ${CMAKE_SOURCE_DIR}/engine/src/vendor/stb_image_impl.cpp ${CMAKE_SOURCE_DIR}/engine/src/render/cgltf_impl.cpp)
target_link_libraries(md_session_synthetic_test PRIVATE md_character)
set_target_properties(md_session_synthetic_test PROPERTIES CXX_STANDARD 17)
add_test(NAME session_synthetic COMMAND md_session_synthetic_test)

# #196: reader of the FCS .def files (editor/data/fcs_def.*), tested on synthetic mini files: ninja md_fcs_def_test && ctest -R fcs_def (KENSHI_DIR set: also parses the install's real files)
add_library(md_fcs_def STATIC editor/data/fcs_def.cpp editor/data/data_browser.cpp editor/data/mod_edit.cpp editor/data/mod_write.cpp editor/data/data_panel.cpp)
target_include_directories(md_fcs_def PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/editor/data)
target_link_libraries(md_fcs_def PUBLIC monkey_dust::engine)
target_include_directories(md_fcs_def PRIVATE ${CMAKE_SOURCE_DIR}/tools/third_party/imgui)
set_target_properties(md_fcs_def PROPERTIES CXX_STANDARD 17)
add_executable(md_fcs_def_test tests/fcs_def_test.cpp ${CMAKE_SOURCE_DIR}/engine/src/vendor/stb_image_impl.cpp ${CMAKE_SOURCE_DIR}/engine/src/render/cgltf_impl.cpp)
target_link_libraries(md_fcs_def_test PRIVATE md_fcs_def)
set_target_properties(md_fcs_def_test PROPERTIES CXX_STANDARD 17)
add_test(NAME fcs_def COMMAND md_fcs_def_test)

# ── #189 B: libraries shared by the game and the editor ───────────────────────────────────────────────────────────────────────────────────────────
# Built here (or prebuilt by this repo's CI as a package, MonkeyDustTools): the game (private main repo) links them instead of compiling ~30 ImGui / panel
# translation units itself. md_ui_third_party = Dear ImGui + backends + its extensions; md_game_panels = the editor panels as the game embeds them
# (MONKEY_DUST_EDITOR, no MONKEY_DUST_STANDALONE_EDITOR; the game-coupled panel game/src/editor/editor_inspector.cpp stays in the game). Link md_game_panels as a
# whole archive: the panels register themselves through static initialisers.
set(MD_MARCH "native" CACHE STRING "-march= for the tools libraries and the editor (CI: skylake; a native build on a runner of another CPU family crashed with SIGILL on the laptop)")
set(MD_TP ${CMAKE_SOURCE_DIR}/tools/third_party)
add_library(md_ui_third_party STATIC
    ${MD_TP}/imgui/imgui.cpp ${MD_TP}/imgui/imgui_draw.cpp ${MD_TP}/imgui/imgui_tables.cpp ${MD_TP}/imgui/imgui_widgets.cpp
    ${MD_TP}/imgui/backends/imgui_impl_sdl3.cpp ${MD_TP}/imgui/backends/imgui_impl_sdlgpu3.cpp
    ${MD_TP}/imgui-node-editor/imgui_node_editor.cpp ${MD_TP}/imgui-node-editor/imgui_node_editor_api.cpp ${MD_TP}/imgui-node-editor/imgui_canvas.cpp ${MD_TP}/imgui-node-editor/crude_json.cpp
    ${MD_TP}/imguizmo/src/ImSequencer.cpp ${MD_TP}/imguizmo/src/ImGradient.cpp
    ${MD_TP}/imgui-flame-graph/imgui_widget_flamegraph.cpp ${MD_TP}/ImGuiColorTextEdit/TextEditor.cpp ${MD_TP}/imnodes/imnodes.cpp)
target_include_directories(md_ui_third_party PUBLIC
    ${MD_TP}/imgui ${MD_TP}/imgui/backends ${MD_TP}/imgui-node-editor ${MD_TP}/ImGuiColorTextEdit ${MD_TP}/imnodes ${MD_TP}/imguizmo/src ${MD_TP}/imgui-flame-graph
    ${MD_TP}/imgui-command-palette)
target_link_libraries(md_ui_third_party PUBLIC monkey_dust::engine $<IF:$<BOOL:${MD_STATIC_DEPS}>,SDL3::SDL3-static,SDL3::SDL3>)
set_source_files_properties(${MD_TP}/imgui-flame-graph/imgui_widget_flamegraph.cpp ${MD_TP}/imgui-node-editor/imgui_node_editor_api.cpp ${MD_TP}/imgui-node-editor/imgui_canvas.cpp
    PROPERTIES COMPILE_DEFINITIONS "IMGUI_DEFINE_MATH_OPERATORS")
set_target_properties(md_ui_third_party PROPERTIES POSITION_INDEPENDENT_CODE OFF)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/fonts_embed.cpp PROPERTIES
    COMPILE_DEFINITIONS "MD_FONT_DIR=\"${MD_FONT_DIR}\""
    OBJECT_DEPENDS "${MD_FONT_TTFS}")
add_library(md_game_panels STATIC
    fonts_embed.cpp
    editor/editor_core.cpp editor/editor_screenshot.cpp editor/editor_animation_panel.cpp editor/editor_camera_panel.cpp editor/editor_command_palette.cpp
    editor/editor_console.cpp editor/editor_flowgraph_panel.cpp editor/editor_gpu_profiler_panel.cpp editor/editor_hierarchy.cpp editor/editor_sequencer_panel.cpp
    editor/editor_std_commands.cpp editor/editor_toolbar.cpp editor/editor_world_3d_sdlgpu.cpp)
target_compile_definitions(md_game_panels PUBLIC MONKEY_DUST_EDITOR DEBUG)
target_include_directories(md_game_panels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/editor ${CMAKE_SOURCE_DIR}/engine/src/render ${CMAKE_SOURCE_DIR}/engine/src/vendor)
target_link_libraries(md_game_panels PUBLIC md_ui_third_party monkey_dust::engine)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # the flags the game gave these sources before (-ffast-math: game/CMakeLists.txt, RE note there)
    target_compile_options(md_ui_third_party PRIVATE -O2 -march=${MD_MARCH})
    target_compile_options(md_game_panels PRIVATE -O2 -march=${MD_MARCH} -ffast-math)
endif()
target_precompile_headers(md_game_panels PRIVATE <cstdint> <cstdlib> <cstring> <cstdio> <cmath>)

# package (component md_tools): the static libraries; headers stay in the tools submodule that main has checked out at the same commit
install(TARGETS md_ui_third_party md_game_panels md_character ARCHIVE DESTINATION lib COMPONENT md_tools)
execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse HEAD OUTPUT_VARIABLE MD_TOOLS_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT MD_TOOLS_SHA)
    set(MD_TOOLS_SHA "unknown")
endif()
foreach(_t md_ui_third_party md_game_panels md_character)
    set(MD_TOOLS_FILE_${_t} "$<TARGET_FILE_NAME:${_t}>")
endforeach()
configure_file(ci/MonkeyDustToolsConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/MonkeyDustToolsConfig.in.cmake" @ONLY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/MonkeyDustToolsConfig.cmake" INPUT "${CMAKE_CURRENT_BINARY_DIR}/MonkeyDustToolsConfig.in.cmake")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/MonkeyDustToolsConfig.cmake" DESTINATION lib/cmake/MonkeyDustTools COMPONENT md_tools)

# ── monkey_dust_editor ────────────────────────────────────
# Standalone 3D editor. UI: Dear ImGui via imgui_impl_sdl3/sdlgpu3 (SDL_GPU=Vulkan).
# Same backend as the game — enables full TerrainRenderer/PropRenderer reuse.
# Single binary — no dlopen/hot-reload .so (removed 2026-09-17, owner
# decision: repeated stale-.so debugging confusion outweighed the ~5-10s
# panels-only rebuild it saved -- see tools/editor/CLAUDE.md).

# All editor panel .cpp files (all wicked-style panels).
file(GLOB EDITOR_PANEL_SOURCES "editor/*.cpp")
list(REMOVE_ITEM EDITOR_PANEL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/editor/main.cpp")

set(EDITOR_IMGUI_SRC
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/backends/imgui_impl_sdl3.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/backends/imgui_impl_sdlgpu3.cpp
)

# RENDER-BACKEND-STAGE-6 (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.3):
# permanent Granite+ImGui bridge (tools/editor/editor_granite_imgui_bridge.cpp)
# needs imgui's raw-Vulkan backend (NOT imgui_impl_sdlgpu3.cpp above, which
# only knows SDL_GPU's opaque handles) and real Vulkan headers for
# ImGui_ImplVulkan_InitInfo's VkInstance/VkDevice/etc fields. Zero cost when
# USE_GRANITE=OFF (the default) -- matches every other USE_GRANITE-gated
# block in this repo (root CMakeLists.txt, engine/CMakeLists.txt).
if(USE_GRANITE)
    find_package(Vulkan REQUIRED)
    list(APPEND EDITOR_IMGUI_SRC ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/backends/imgui_impl_vulkan.cpp)
endif()

if(NOT MONKEY_DUST_TOOLS_NEED_SDL3)
    message(STATUS "[tools] SDL3 disabled — skipping monkey_dust_editor")
    # Only flare_convert builds without SDL3
else()

set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/fonts_embed.cpp PROPERTIES
    COMPILE_DEFINITIONS "MD_FONT_DIR=\"${MD_FONT_DIR}\""
    OBJECT_DEPENDS "${MD_FONT_TTFS}")

add_executable(monkey_dust_editor
    editor/main.cpp
    fonts_embed.cpp
    # Header-only files: listed here so ninja tracks them and rebuilds on change
    editor/editor_char_preview.h
    editor/character_editor.h
    ${EDITOR_PANEL_SOURCES}
    ${EDITOR_IMGUI_SRC}
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/stb_image_impl.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/render/cgltf_impl.cpp
    # Third-party panels
    ${CMAKE_SOURCE_DIR}/tools/third_party/imnodes/imnodes.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/ImGuiColorTextEdit/TextEditor.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-flame-graph/imgui_widget_flamegraph.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src/ImSequencer.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src/ImGradient.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_node_editor.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_node_editor_api.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_canvas.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/crude_json.cpp
)
target_compile_definitions(monkey_dust_editor PRIVATE
    MONKEY_DUST_STANDALONE_EDITOR
    MONKEY_DUST_EDITOR
    IMGUI_DEFINE_MATH_OPERATORS
    $<$<BOOL:${USE_GRANITE}>:IMGUI_IMPL_VULKAN_NO_PROTOTYPES>
)
target_link_libraries(monkey_dust_editor PRIVATE
    md_character
    md_fcs_def
    monkey_dust::engine
    $<IF:$<BOOL:${MD_STATIC_DEPS}>,SDL3::SDL3-static,SDL3::SDL3>
    $<$<BOOL:${USE_GRANITE}>:Vulkan::Vulkan>
)
# #192: Characters tab on the original Kenshi window (MyGUI); the library is built by the engine when MD_MYGUI is ON
if(TARGET monkey_dust_mygui OR TARGET monkey_dust::mygui)   # in-tree engine (MD_MYGUI=ON) or the prebuilt package that carries monkey_dust::mygui
    target_link_libraries(monkey_dust_editor PRIVATE $<IF:$<TARGET_EXISTS:monkey_dust_mygui>,monkey_dust_mygui,monkey_dust::mygui>)
    target_compile_definitions(monkey_dust_editor PRIVATE MD_CHARACTER_MYGUI=1)
endif()
target_include_directories(monkey_dust_editor PRIVATE
    editor/
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/backends
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor
    ${CMAKE_SOURCE_DIR}/tools/third_party/ImGuiColorTextEdit
    ${CMAKE_SOURCE_DIR}/tools/third_party/imnodes
    ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-flame-graph
    ${CMAKE_SOURCE_DIR}/engine/src/vendor
    ${CMAKE_SOURCE_DIR}/engine/src/render
)
# (third-party sources already added above)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # CI builds on a runner CPU of another family (AVX-512 there): -DMD_MARCH=skylake keeps the binary runnable on the target laptop (i5-6200U, Skylake)
    target_compile_options(monkey_dust_editor PRIVATE -O2 -march=${MD_MARCH})
endif()


# ── PCH for editor own sources ───────────────────────────────────────────────
# PCH on engine lib (PRIVATE) doesn't propagate to editor .cpp files.
# We add it separately here. Third-party sources (imgui-node-editor, imnodes,
# ImGuiColorTextEdit, imguizmo) are excluded via SET_SOURCE_FILES_PROPERTIES
# SKIP_PRECOMPILE_HEADERS to avoid conflicts with their own internal defines.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
    target_precompile_headers(monkey_dust_editor PRIVATE
        <cstdint> <cstdlib> <cstring> <cstdio>
        <imgui.h>
        <SDL3/SDL_gpu.h>
    )
    md_gaia_pch(monkey_dust_editor)
    # Third-party ImGui panels have conflicting macros — skip PCH for them.
    set(_EDITOR_THIRDPARTY_CPP
        ${CMAKE_SOURCE_DIR}/tools/third_party/imnodes/imnodes.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/ImGuiColorTextEdit/TextEditor.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-flame-graph/imgui_widget_flamegraph.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src/ImSequencer.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src/ImGradient.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_node_editor.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_node_editor_api.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_canvas.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/crude_json.cpp
        ${CMAKE_SOURCE_DIR}/engine/src/vendor/stb_image_impl.cpp
        ${CMAKE_SOURCE_DIR}/engine/src/render/cgltf_impl.cpp
    )
    set_source_files_properties(${_EDITOR_THIRDPARTY_CPP} PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON)
    message(STATUS "[monkey_dust_editor] PCH: ENABLED (imgui + SDL_GPU)")
endif()

endif() # MONKEY_DUST_TOOLS_NEED_SDL3 — editor ends here

# ── kenshi_navmesh_extract ────────────────────────────────
# Standalone Havok .hkt navmesh -> OBJ geometry extractor — no SDL3, no
# engine. See tools/third_party/hkxparse/README_MONKEYDUST.md for the parser
# reverse-engineering writeup (Kenshi's real per-zone navmesh geometry,
# extracted from game data/newland/land/navtiles/tile*.hkt).
add_executable(kenshi_navmesh_extract
    kenshi_navmesh_extract/main.cpp
    third_party/hkxparse/src/HKXMapping.cpp
    third_party/hkxparse/src/HKXTagfileParser.cpp
    third_party/hkxparse/src/HKXTypes.cpp
    third_party/hkxparse/src/Deserializer.cpp
)
target_include_directories(kenshi_navmesh_extract PRIVATE
    third_party/hkxparse/include
)
target_compile_options(kenshi_navmesh_extract PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hkxparse/include/hkxparse/msvc_compat.h")
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(kenshi_navmesh_extract PRIVATE -O2)
endif()
