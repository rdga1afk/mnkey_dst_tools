# Unit tests of our code (tools repo). Used standalone (top project) and from the private main repo (tests/CMakeLists.txt adds this directory).
# MD_ROOT = the project root (main, or the tools checkout when standalone): the tests run with it as the working directory.
if(NOT MD_ROOT)
    set(MD_ROOT ${CMAKE_SOURCE_DIR})
endif()
set(MD_UTEST_DIR ${CMAKE_CURRENT_LIST_DIR})
find_package(GTest REQUIRED)
option(MONKEY_DUST_GAME_TESTS_ONLY "Build only game-specific tests" OFF)

if(NOT MONKEY_DUST_GAME_TESTS_ONLY)

add_executable(md_tests
    ${MD_UTEST_DIR}/test_health_make.cpp
    ${MD_UTEST_DIR}/test_md_camera_translate.cpp
    ${MD_UTEST_DIR}/test_gaia_each_bench.cpp
    ${MD_UTEST_DIR}/test_global_component_warmup.cpp
    ${MD_UTEST_DIR}/test_fnv.cpp
    ${MD_UTEST_DIR}/test_flow_graph.cpp
    ${MD_UTEST_DIR}/test_granite_backend.cpp
    ${MD_UTEST_DIR}/lua_sandbox_test.cpp
    ${MD_UTEST_DIR}/test_kenshi_sky.cpp
    ${MD_UTEST_DIR}/test_md_batch14.cpp
    ${MD_UTEST_DIR}/test_behavior_tree_md.cpp
    ${MD_UTEST_DIR}/test_md_batch13.cpp
    ${MD_UTEST_DIR}/test_md_batch16.cpp
    ${MD_UTEST_DIR}/test_md_m53.cpp
    ${MD_UTEST_DIR}/test_md_m54.cpp
    ${MD_UTEST_DIR}/test_md_m55.cpp
    ${MD_UTEST_DIR}/test_md_m56.cpp
    ${MD_UTEST_DIR}/test_md_m58.cpp
    ${MD_UTEST_DIR}/test_light_system_defaults.cpp
    ${MD_UTEST_DIR}/test_md_borg6.cpp
    ${MD_UTEST_DIR}/test_md_terrain.cpp
    ${MD_UTEST_DIR}/test_job_graph.cpp
    ${MD_UTEST_DIR}/test_job_graph_stress.cpp
    ${MD_UTEST_DIR}/test_gaia_sched_adapter.cpp
    ${MD_UTEST_DIR}/test_gaia_sparse_storage.cpp
    ${MD_UTEST_DIR}/test_md_registry.cpp
    ${MD_UTEST_DIR}/test_transform_soa_simd_parity.cpp
    ${MD_UTEST_DIR}/test_set_stage_count_bench.cpp
    ${MD_UTEST_DIR}/stress_gpu_resource_cycle.cpp
    ${MD_UTEST_DIR}/test_terrain_worldmap_normal_bake.cpp
    ${MD_UTEST_DIR}/test_terrain_quadtree.cpp
    ${MD_UTEST_DIR}/test_terrain_quadtree_renderer.cpp
    ${MD_UTEST_DIR}/test_terrain_tile.cpp
    ${MD_UTEST_DIR}/test_editor_cmd_registry.cpp
    ${MD_UTEST_DIR}/test_cmd_path_validate.cpp
    ${MD_UTEST_DIR}/test_lua_sub_namespace.cpp
    ${MD_UTEST_DIR}/test_toroidal_update.cpp
    ${MD_UTEST_DIR}/test_gpu_pipeline_safety_check.cpp
    ${MD_UTEST_DIR}/test_fcs.cpp
    ${MD_UTEST_DIR}/test_zone_buildings.cpp
    ${MD_UTEST_DIR}/test_foliage_scatter.cpp
    ${MD_UTEST_DIR}/test_gbuf_normal.cpp
    ${MD_UTEST_DIR}/test_derived_cache_gc.cpp
    ${MD_UTEST_DIR}/test_biome_fcs.cpp
    ${MD_UTEST_DIR}/test_tiff.cpp
    ${MD_UTEST_DIR}/test_kenshi_dir.cpp
    ${MD_UTEST_DIR}/test_data_files.cpp
    ${MD_UTEST_DIR}/test_biome_map_install.cpp
    ${MD_UTEST_DIR}/test_feature_scatter_install.cpp
    ${MD_UTEST_DIR}/test_kbi1_lookup_install.cpp
    ${MD_UTEST_DIR}/test_ogre_static_mesh.cpp
    ${MD_UTEST_DIR}/test_kenshi_loading_screen.cpp
    ${MD_UTEST_DIR}/test_human_body_build.cpp
    ${MD_UTEST_DIR}/test_dds_decode.cpp
    ${MD_UTEST_DIR}/test_overlay_mask_build.cpp
    ${MD_UTEST_DIR}/test_steepness_bake.cpp
    ${MD_UTEST_DIR}/test_terrain_colour_dds.cpp
    ${MD_UTEST_DIR}/test_ground_bake.cpp
    ${MD_UTEST_DIR}/test_ogre.cpp
    ${MD_UTEST_DIR}/test_kenshi_land.cpp
    ${MD_UTEST_DIR}/test_weather.cpp
    ${MD_UTEST_DIR}/test_biome_layer_cache.cpp
    ${MD_UTEST_DIR}/test_kbi1.cpp
    ${MD_UTEST_DIR}/test_biome_layer_arrays_gpu.cpp
    ${MD_UTEST_DIR}/test_frame_counters.cpp
)

target_sources(md_tests PRIVATE
    ${MD_ROOT}/engine/src/vendor/stb_image_impl.cpp
    ${MD_ROOT}/engine/src/render/cgltf_impl.cpp
)

target_link_libraries(md_tests PRIVATE
    monkey_dust::engine
    GTest::gtest_main
)

md_gaia_pch(md_tests)

include(GoogleTest)
# tests that ASSERT a Kenshi install (no skip, the lint forbids GTEST_SKIP): label needs-install, not fast; CI without an install runs `ctest -L fast -LE needs-install`
gtest_discover_tests(md_tests WORKING_DIRECTORY ${MD_ROOT} TEST_FILTER -BiomeMapInstall.EveryCellIsTheRecordThatOwnsItsColour:ZonesFromInstall.ExactSetOfGroupsWithARegion:OgreStaticMesh.HairGlbsEqualDirectMesh:OgreStaticMesh.HairRawExtents PROPERTIES LABELS "fast" TIMEOUT 120)
gtest_discover_tests(md_tests WORKING_DIRECTORY ${MD_ROOT} TEST_FILTER BiomeMapInstall.EveryCellIsTheRecordThatOwnsItsColour:ZonesFromInstall.ExactSetOfGroupsWithARegion:OgreStaticMesh.HairGlbsEqualDirectMesh:OgreStaticMesh.HairRawExtents TEST_LIST md_install_tests PROPERTIES LABELS "needs-install" TIMEOUT 120)

# FCS dump tool: C++ reader vs the Python extraction (tests/verify_fcs_vs_db.py). Development only.
add_executable(md_fcsdump ${MD_UTEST_DIR}/fcsdump.cpp
    ${MD_ROOT}/engine/src/vendor/stb_image_impl.cpp
    ${MD_ROOT}/engine/src/render/cgltf_impl.cpp)
target_link_libraries(md_fcsdump PRIVATE monkey_dust::engine)

endif() # NOT MONKEY_DUST_GAME_TESTS_ONLY

# #159 oracles (the data gate script is in the private repo): md::character dumps
if(TARGET md_character)
    add_executable(md_character_dump ${MD_UTEST_DIR}/character_dump.cpp
        ${MD_ROOT}/engine/src/vendor/stb_image_impl.cpp
        ${MD_ROOT}/engine/src/render/cgltf_impl.cpp)
    target_link_libraries(md_character_dump PRIVATE md_character)
    add_executable(md_character_randomise_test ${MD_UTEST_DIR}/character_randomise_test.cpp)
    target_link_libraries(md_character_randomise_test PRIVATE md_character)
    add_executable(md_character_hair_census ${MD_UTEST_DIR}/character_hair_census.cpp)
    target_link_libraries(md_character_hair_census PRIVATE md_character)
endif()
