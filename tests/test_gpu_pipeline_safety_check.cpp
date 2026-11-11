#include <gtest/gtest.h>
#include <monkey_dust/render/gpu_pipeline_safety_check.h>
#include <string>

// docs/archive/DAGOR_IMPLEMENTATION_PROMPT.md Рівень 2, КРОК 6 -- acceptance
// criterion: "штучний тест -- навмисно скласти Desc з відомо-небезпечною
// комбінацією (semplers+storage buffer+storage texture одночасно) --
// перевірка ловить це ДО виклику Create(), не живим крашем."

using md::ValidateComputePipelineDesc;
using md::ValidateGraphicsPipelineDesc;

TEST(GpuPipelineSafetyCheck, CatchesTask141KnownUnsafeComboReadonlyBuffer) {
    // The exact shape that crashed libvulkan_intel.so twice under task
    // #141: a readonly storage buffer mixed with samplers AND a storage
    // texture in one compute pipeline.
    GpuComputePipeline::Desc desc;
    desc.num_uniform_buffers = 1;
    desc.num_readonly_storage_buffers = 1;
    desc.num_samplers = 1;
    desc.num_readwrite_storage_textures = 1;

    const char* reason = nullptr;
    EXPECT_FALSE(ValidateComputePipelineDesc(desc, &reason));
    ASSERT_NE(reason, nullptr);
    EXPECT_NE(std::string(reason).find("storage buffer"), std::string::npos);
}

TEST(GpuPipelineSafetyCheck, CatchesReadwriteBufferPlusSamplerCombo) {
    GpuComputePipeline::Desc desc;
    desc.num_readwrite_storage_buffers = 1;
    desc.num_samplers = 1;

    const char* reason = nullptr;
    EXPECT_FALSE(ValidateComputePipelineDesc(desc, &reason));
    ASSERT_NE(reason, nullptr);
}

TEST(GpuPipelineSafetyCheck, AllowsKnownSafeSamplerPlusStorageTexturePattern) {
    // terrain_worldmap_normal_bake.comp's real, working shape: samplers +
    // 1 rw-storage-texture + UBO, no storage buffers at all.
    GpuComputePipeline::Desc desc;
    desc.num_uniform_buffers = 1;
    desc.num_samplers = 2;
    desc.num_readwrite_storage_textures = 1;

    const char* reason = nullptr;
    EXPECT_TRUE(ValidateComputePipelineDesc(desc, &reason));
}

TEST(GpuPipelineSafetyCheck, AllowsKnownSafeStorageBufferOnlyPattern) {
    // npc_cull.comp's real, working shape: storage buffers + UBO, no
    // samplers/textures at all.
    GpuComputePipeline::Desc desc;
    desc.num_uniform_buffers = 1;
    desc.num_readonly_storage_buffers = 2;
    desc.num_readwrite_storage_buffers = 1;

    const char* reason = nullptr;
    EXPECT_TRUE(ValidateComputePipelineDesc(desc, &reason));
}

TEST(GpuPipelineSafetyCheck, FlagsMoreThanOneReadwriteStorageTextureAsUntested) {
    GpuComputePipeline::Desc desc;
    desc.num_readwrite_storage_textures = 2;

    const char* reason = nullptr;
    EXPECT_FALSE(ValidateComputePipelineDesc(desc, &reason));
    ASSERT_NE(reason, nullptr);
    EXPECT_NE(std::string(reason).find("NOT a confirmed crash"), std::string::npos);
}

TEST(GpuPipelineSafetyCheck, GraphicsCatchesFragSamplerPlusVertStorageBufSilentFail) {
    GpuPipeline::Desc desc;
    desc.frag_samplers = 1;
    desc.vert_storage_bufs = 1;

    const char* reason = nullptr;
    EXPECT_FALSE(ValidateGraphicsPipelineDesc(desc, &reason));
    ASSERT_NE(reason, nullptr);
}

TEST(GpuPipelineSafetyCheck, GraphicsAllowsFragSamplerAloneNoVertStorageBuf) {
    GpuPipeline::Desc desc;
    desc.frag_samplers = 1;
    desc.vert_storage_bufs = 0;

    const char* reason = nullptr;
    EXPECT_TRUE(ValidateGraphicsPipelineDesc(desc, &reason));
}
