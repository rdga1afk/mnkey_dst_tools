# Unit tests of our code (tools repo). Used standalone (top project) and from the private main repo (tests/CMakeLists.txt adds this directory).
# MD_ROOT = the project root (main, or the tools checkout when standalone): the tests run with it as the working directory.
if(NOT MD_ROOT)
    set(MD_ROOT ${CMAKE_SOURCE_DIR})
endif()
set(MD_UTEST_DIR ${CMAKE_CURRENT_LIST_DIR})
find_package(GTest REQUIRED)
option(MONKEY_DUST_GAME_TESTS_ONLY "Build only game-specific tests" OFF)

if(NOT MONKEY_DUST_GAME_TESTS_ONLY)

# the vertex shader's border-snap lines compiled as C++ for LodPluginOracle.ShaderEdgeSnapEqualsTheCppTwin (the rule is checked on the shader's own text)
set(MD_LOD_SHADER ${MD_UTEST_DIR}/../shaders/terrain_quadtree_batched_lod.vert)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MD_LOD_SHADER})
file(READ ${MD_LOD_SHADER} _md_lod_vert)
string(REGEX MATCH "// EDGE-SNAP-BEGIN[^\n]*\n(.*)// EDGE-SNAP-END" _md_unused "${_md_lod_vert}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/edge_snap_shader.inc "${CMAKE_MATCH_1}")
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
    ${MD_UTEST_DIR}/test_kenshi_save.cpp
    ${MD_UTEST_DIR}/test_town_spawn.cpp
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
    ${MD_UTEST_DIR}/test_reflect_camera.cpp
    ${MD_UTEST_DIR}/test_csm_splits.cpp
    ${MD_UTEST_DIR}/test_grass_placement.cpp
    ${MD_UTEST_DIR}/test_grass_pages.cpp
    ${MD_UTEST_DIR}/test_pu_script.cpp
    ${MD_UTEST_DIR}/test_pu_system.cpp
    ${MD_UTEST_DIR}/test_lod_state.cpp
    ${MD_UTEST_DIR}/test_lod_plugin_oracle.cpp
    ${MD_UTEST_DIR}/test_flip_back_check.cpp
    ${MD_UTEST_DIR}/test_inverse_check.cpp
    ${MD_UTEST_DIR}/test_kenshi_land.cpp
    ${MD_UTEST_DIR}/test_weather.cpp
    ${MD_UTEST_DIR}/test_install_hash.cpp
    ${MD_UTEST_DIR}/test_player_selection.cpp
    ${MD_UTEST_DIR}/test_move_speed.cpp
    ${MD_UTEST_DIR}/test_combat_hit.cpp
    ${MD_UTEST_DIR}/test_steer.cpp
    ${MD_UTEST_DIR}/test_start_squad.cpp
    ${MD_UTEST_DIR}/test_slice_squad.cpp
    ${MD_UTEST_DIR}/test_neighbour_avoid.cpp
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
target_include_directories(md_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(md_tests PRIVATE MD_TOOLS_TEST_DATA="${MD_UTEST_DIR}/data")   # the emulator vectors: same path in main and standalone mode

md_gaia_pch(md_tests)

include(GoogleTest)
# tests that ASSERT a Kenshi install (no skip, the lint forbids GTEST_SKIP): label needs-install, not fast; CI without an install runs `ctest -L fast -LE needs-install`
gtest_discover_tests(md_tests WORKING_DIRECTORY ${MD_ROOT} TEST_FILTER -StartSquad.CannibalHuntersMergedLoadOrder:TownSpawnInstall.HubHasRebirthResidentsAndBrinkHasUnitedCitiesResidents:TownSpawnInstall.HubHasGeneralNodesInsideTheTownRadius:TownSpawnInstall.HubPlanIsPlacedOnNodesAndSeedStable:FeatureScatterInstall.OutputMatchesHashReference:Kbi1LookupInstall.OutputMatchesHashReference:OgreStaticMesh.FeatureMeshesMatchHashReference:DdsDecode.BodyTextureMatchesHashReference:OverlayMaskBuild.OutputMatchesHashReference:SteepnessBake.OutputMatchesHashReference:TerrainColourDds.OutputMatchesHashReference:HumanBodyBuild.OutputMatchesHashReference:BiomeFcs.TableMatchesHashReference:BiomeFcs.RegistryMatchesHashReference:TiffData.FullmapSamplesMatchHashReference:TiffData.AtlasHeightsMatchHashReference:BiomeMapInstall.EveryCellIsTheRecordThatOwnsItsColour:ZonesFromInstall.ExactSetOfGroupsWithARegion:OgreStaticMesh.HairGlbsEqualDirectMesh:OgreStaticMesh.HairRawExtents PROPERTIES LABELS "fast" TIMEOUT 120)
gtest_discover_tests(md_tests WORKING_DIRECTORY ${MD_ROOT} TEST_FILTER StartSquad.CannibalHuntersMergedLoadOrder:TownSpawnInstall.HubHasRebirthResidentsAndBrinkHasUnitedCitiesResidents:TownSpawnInstall.HubHasGeneralNodesInsideTheTownRadius:TownSpawnInstall.HubPlanIsPlacedOnNodesAndSeedStable:FeatureScatterInstall.OutputMatchesHashReference:Kbi1LookupInstall.OutputMatchesHashReference:OgreStaticMesh.FeatureMeshesMatchHashReference:DdsDecode.BodyTextureMatchesHashReference:OverlayMaskBuild.OutputMatchesHashReference:SteepnessBake.OutputMatchesHashReference:TerrainColourDds.OutputMatchesHashReference:HumanBodyBuild.OutputMatchesHashReference:BiomeFcs.TableMatchesHashReference:BiomeFcs.RegistryMatchesHashReference:TiffData.FullmapSamplesMatchHashReference:TiffData.AtlasHeightsMatchHashReference:BiomeMapInstall.EveryCellIsTheRecordThatOwnsItsColour:ZonesFromInstall.ExactSetOfGroupsWithARegion:OgreStaticMesh.HairGlbsEqualDirectMesh:OgreStaticMesh.HairRawExtents TEST_LIST md_install_tests PROPERTIES LABELS "needs-install" TIMEOUT 120)

# Hash-reference tests (coordinator 18.11): the 12 *HashReference tests build their output from the game folder and compare its SHA-256 with tests/data/install_hashes.tsv; they ASSERT the folder and FAIL on an unknown game build.
# Skips that stay by design (each one line, reason):
#   GaiaBench.QueryEach                                    disabled micro-benchmark (run by hand with --gtest_also_run_disabled_tests)
#   GlobalComponentWarmup.PreviouslyMissingComponentsAreRegistered   flecs-specific by-name lookup check, not meaningful under the gaia backend
#   SetStageCountBench.SkippedUnderGaiaBackend             benchmark of the flecs backend
#   TerrainQuadtreeRendererTest.SingleNodeRendersFiniteInBoundsGBuffer   needs the offline biome table (BiomeRegistry::LoadFromFile), gone with #201
#   ZoneBuildings.RankTownsBy300mParts                     research dump, runs only with MD_TOWN_RANK=<out file>
#   FoliageScatter.RankZonesByMediumPlaces                 research dump, runs only with MD_FOLIAGE_RANK=<out file>
#   FoliageScatter.DumpMeshMaterialTypes                   research dump, runs only with MD_FOLIAGE_DUMP=<out file>
#   FoliageScatter.ScanGrassyCircles / ScanGrassColourMaps / ScanGrassSpriteTrim   research scans, run only with MD_GRASS_SCAN=1
#   GroundBake.DumpInputsForPython / MatchesPython / Bench   need MD_GB_DUMP / MD_GB_REF / MD_GB_BENCH (comparison with the python baker, bench)

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
    add_executable(md_character_hair_state_test ${MD_UTEST_DIR}/character_hair_state_test.cpp)
    target_link_libraries(md_character_hair_state_test PRIVATE md_character)
    add_executable(md_character_texture_stats ${MD_UTEST_DIR}/character_texture_stats.cpp
        ${MD_ROOT}/engine/src/vendor/stb_image_impl.cpp ${MD_ROOT}/engine/src/render/cgltf_impl.cpp)
    target_link_libraries(md_character_texture_stats PRIVATE md_character)
    add_executable(md_npc_hair_mesh_test ${MD_UTEST_DIR}/test_npc_hair_mesh.cpp ${MD_ROOT}/tools/game/src/render/npc_hair_mesh.cpp)
    target_include_directories(md_npc_hair_mesh_test PRIVATE ${MD_ROOT}/tools/game/src/render)
    target_link_libraries(md_npc_hair_mesh_test PRIVATE md_character)
    add_test(NAME NpcHairMeshInstall COMMAND sh -c "test -n \"$KENSHI_DIR\" && exec $<TARGET_FILE:md_npc_hair_mesh_test>")
    set_tests_properties(NpcHairMeshInstall PROPERTIES LABELS "needs-install" TIMEOUT 120)
    add_test(NAME CharacterHairState COMMAND sh -c "test -n \"$KENSHI_DIR\" && exec $<TARGET_FILE:md_character_hair_state_test> \"$KENSHI_DIR\"")
    set_tests_properties(CharacterHairState PROPERTIES LABELS "needs-install" TIMEOUT 300)
endif()
if(TARGET monkey_dust_engine)   # Hi-Z step 0: occlusion upper bound of the terrain nodes (CPU, install heightmap)
    add_executable(md_terrain_occlusion_bound ${MD_UTEST_DIR}/terrain_occlusion_bound.cpp ${MD_ROOT}/engine/src/vendor/stb_image_impl.cpp ${MD_ROOT}/engine/src/render/cgltf_impl.cpp)
    target_link_libraries(md_terrain_occlusion_bound PRIVATE monkey_dust_engine)
endif()
