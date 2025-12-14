// Deferred lighting pass implementation

#include "VulkanDeferredLightingPass.h"

#include "VulkanRenderer.h"
#include "VulkanFrame.h"
#include "VulkanPipelineManager.h"

#include <array>
#include <cmath>
#include <utility>

namespace graphics::vulkan {

namespace {

struct Pos3 {
    float x, y, z;
};

vertex_layout buildPositionOnlyLayout()
{
    vertex_layout layout;
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(Pos3), 0);
    return layout;
}

constexpr float kPi = 3.14159265358979323846f;

void generateSphere(uint32_t slices, uint32_t stacks, std::vector<Pos3>& outVerts, std::vector<uint32_t>& outIndices)
{
    outVerts.clear();
    outIndices.clear();

    const uint32_t vertCols = slices + 1;
    outVerts.reserve((stacks + 1) * vertCols);

    for (uint32_t stack = 0; stack <= stacks; ++stack) {
        const float v = static_cast<float>(stack) / static_cast<float>(stacks);
        const float phi = v * kPi;
        const float y = std::cos(phi);
        const float r = std::sin(phi);

        for (uint32_t slice = 0; slice <= slices; ++slice) {
            const float u = static_cast<float>(slice) / static_cast<float>(slices);
            const float theta = u * kPi * 2.0f;
            outVerts.push_back(Pos3{r * std::cos(theta), y, r * std::sin(theta)});
        }
    }

    for (uint32_t stack = 0; stack < stacks; ++stack) {
        for (uint32_t slice = 0; slice < slices; ++slice) {
            const uint32_t a = stack * vertCols + slice;
            const uint32_t b = a + 1;
            const uint32_t c = (stack + 1) * vertCols + slice;
            const uint32_t d = c + 1;

            outIndices.push_back(a);
            outIndices.push_back(c);
            outIndices.push_back(b);
            outIndices.push_back(b);
            outIndices.push_back(c);
            outIndices.push_back(d);
        }
    }
}

void generateCylinder(uint32_t slices, std::vector<Pos3>& outVerts, std::vector<uint32_t>& outIndices)
{
    outVerts.clear();
    outIndices.clear();

    // Unit cylinder aligned to +Z, centered at origin: z in [-0.5, +0.5], radius 1.
    const uint32_t ringVerts = slices + 1;
    outVerts.reserve(ringVerts * 2);

    for (uint32_t slice = 0; slice <= slices; ++slice) {
        const float u = static_cast<float>(slice) / static_cast<float>(slices);
        const float theta = u * kPi * 2.0f;
        const float x = std::cos(theta);
        const float y = std::sin(theta);

        outVerts.push_back(Pos3{x, y, -0.5f});
        outVerts.push_back(Pos3{x, y, 0.5f});
    }

    for (uint32_t slice = 0; slice < slices; ++slice) {
        const uint32_t i0 = slice * 2;
        const uint32_t i1 = i0 + 1;
        const uint32_t i2 = (slice + 1) * 2;
        const uint32_t i3 = i2 + 1;

        outIndices.push_back(i0);
        outIndices.push_back(i1);
        outIndices.push_back(i2);
        outIndices.push_back(i2);
        outIndices.push_back(i1);
        outIndices.push_back(i3);
    }
}

} // namespace

// -----------------------------
// Light recorders
// -----------------------------

void FullscreenLight::record(vk::CommandBuffer cmd, VulkanFrame& frame, const DeferredLightingSharedState& shared)
{
    auto alloc = frame.uniformBuffer().allocate(sizeof(DeferredLightUBO), 256);
    std::memcpy(alloc.mapped, &ubo, sizeof(DeferredLightUBO));

    const uint32_t dynOffset = static_cast<uint32_t>(alloc.offset);

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, shared.fullscreenPipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
        shared.pass.m_pipelineLayout.get(),
        0, 1, &shared.pass.m_lightSet, 1, &dynOffset);

    // Fullscreen triangle using gl_VertexIndex
    cmd.draw(3, 1, 0, 0);
}

void SphereLight::record(vk::CommandBuffer cmd, VulkanFrame& frame, const DeferredLightingSharedState& shared)
{
    auto alloc = frame.uniformBuffer().allocate(sizeof(DeferredLightUBO), 256);
    std::memcpy(alloc.mapped, &ubo, sizeof(DeferredLightUBO));

    const uint32_t dynOffset = static_cast<uint32_t>(alloc.offset);

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, shared.volumePipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
        shared.pass.m_pipelineLayout.get(),
        0, 1, &shared.pass.m_lightSet, 1, &dynOffset);

    vk::DeviceSize vbOffset = 0;
    vk::Buffer vb = shared.pass.m_renderer.getBuffer(shared.pass.m_sphere.vertex);
    vk::Buffer ib = shared.pass.m_renderer.getBuffer(shared.pass.m_sphere.index);
    cmd.bindVertexBuffers(0, 1, &vb, &vbOffset);
    cmd.bindIndexBuffer(ib, 0, vk::IndexType::eUint32);
    cmd.drawIndexed(shared.pass.m_sphere.indexCount, 1, 0, 0, 0);
}

void CylinderLight::record(vk::CommandBuffer cmd, VulkanFrame& frame, const DeferredLightingSharedState& shared)
{
    auto alloc = frame.uniformBuffer().allocate(sizeof(DeferredLightUBO), 256);
    std::memcpy(alloc.mapped, &ubo, sizeof(DeferredLightUBO));

    const uint32_t dynOffset = static_cast<uint32_t>(alloc.offset);

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, shared.volumePipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
        shared.pass.m_pipelineLayout.get(),
        0, 1, &shared.pass.m_lightSet, 1, &dynOffset);

    vk::DeviceSize vbOffset = 0;
    vk::Buffer vb = shared.pass.m_renderer.getBuffer(shared.pass.m_cylinder.vertex);
    vk::Buffer ib = shared.pass.m_renderer.getBuffer(shared.pass.m_cylinder.index);
    cmd.bindVertexBuffers(0, 1, &vb, &vbOffset);
    cmd.bindIndexBuffer(ib, 0, vk::IndexType::eUint32);
    cmd.drawIndexed(shared.pass.m_cylinder.indexCount, 1, 0, 0, 0);
}

// -----------------------------
// Pass
// -----------------------------

VulkanDeferredLightingPass::VulkanDeferredLightingPass(VulkanRenderer& renderer) : m_renderer(renderer)
{
    createLightDescriptors();
    createMeshes();

    // Pipeline layout: set0 = per-light dynamic UBO, set1 = global G-buffer descriptors
    std::array<vk::DescriptorSetLayout, 2> setLayouts = {
        m_lightSetLayout.get(),
        m_renderer.getDescriptorLayouts().globalLayout()
    };

    vk::PipelineLayoutCreateInfo pli{};
    pli.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pli.pSetLayouts = setLayouts.data();
    m_pipelineLayout = m_renderer.device().createPipelineLayoutUnique(pli);
}

VulkanDeferredLightingPass::~VulkanDeferredLightingPass()
{
    if (m_sphere.vertex.isValid()) {
        m_renderer.deleteBuffer(m_sphere.vertex);
    }
    if (m_sphere.index.isValid()) {
        m_renderer.deleteBuffer(m_sphere.index);
    }
    if (m_cylinder.vertex.isValid()) {
        m_renderer.deleteBuffer(m_cylinder.vertex);
    }
    if (m_cylinder.index.isValid()) {
        m_renderer.deleteBuffer(m_cylinder.index);
    }
}

void VulkanDeferredLightingPass::createLightDescriptors()
{
    vk::DescriptorSetLayoutBinding lightBinding{};
    lightBinding.binding = 0;
    lightBinding.descriptorCount = 1;
    lightBinding.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
    lightBinding.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

    vk::DescriptorSetLayoutCreateInfo li{};
    li.bindingCount = 1;
    li.pBindings = &lightBinding;
    m_lightSetLayout = m_renderer.device().createDescriptorSetLayoutUnique(li);

    vk::DescriptorPoolSize poolSize{};
    poolSize.type = vk::DescriptorType::eUniformBufferDynamic;
    poolSize.descriptorCount = 1;

    vk::DescriptorPoolCreateInfo pi{};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &poolSize;
    m_lightPool = m_renderer.device().createDescriptorPoolUnique(pi);

    vk::DescriptorSetAllocateInfo ai{};
    ai.descriptorPool = m_lightPool.get();
    ai.descriptorSetCount = 1;
    auto layout = m_lightSetLayout.get();
    ai.pSetLayouts = &layout;

    auto sets = m_renderer.device().allocateDescriptorSets(ai);
    m_lightSet = sets.front();
}

void VulkanDeferredLightingPass::createMeshes()
{
    m_sphere = createUnitSphereMesh(24, 16);
    m_cylinder = createUnitCylinderMesh(24);
}

VulkanDeferredLightingPass::Mesh VulkanDeferredLightingPass::createUnitSphereMesh(uint32_t slices, uint32_t stacks)
{
    std::vector<Pos3> verts;
    std::vector<uint32_t> indices;
    generateSphere(slices, stacks, verts, indices);

    Mesh mesh;
    mesh.layout = buildPositionOnlyLayout();
    mesh.vertex = m_renderer.createBuffer(BufferType::Vertex, BufferUsageHint::Static);
    mesh.index = m_renderer.createBuffer(BufferType::Index, BufferUsageHint::Static);
    mesh.indexCount = static_cast<uint32_t>(indices.size());

    m_renderer.updateBufferData(mesh.vertex, verts.size() * sizeof(Pos3), verts.data());
    m_renderer.updateBufferData(mesh.index, indices.size() * sizeof(uint32_t), indices.data());

    return mesh;
}

VulkanDeferredLightingPass::Mesh VulkanDeferredLightingPass::createUnitCylinderMesh(uint32_t slices)
{
    std::vector<Pos3> verts;
    std::vector<uint32_t> indices;
    generateCylinder(slices, verts, indices);

    Mesh mesh;
    mesh.layout = buildPositionOnlyLayout();
    mesh.vertex = m_renderer.createBuffer(BufferType::Vertex, BufferUsageHint::Static);
    mesh.index = m_renderer.createBuffer(BufferType::Index, BufferUsageHint::Static);
    mesh.indexCount = static_cast<uint32_t>(indices.size());

    m_renderer.updateBufferData(mesh.vertex, verts.size() * sizeof(Pos3), verts.data());
    m_renderer.updateBufferData(mesh.index, indices.size() * sizeof(uint32_t), indices.data());

    return mesh;
}

void VulkanDeferredLightingPass::record(vk::CommandBuffer cmd, VulkanFrame& frame, std::vector<DeferredLight>& lights)
{
    // Ensure swapchain rendering is active
    m_renderer.ensureRenderingStarted(cmd);

    // Dynamic state for lighting pass
    cmd.setDepthTestEnable(VK_FALSE);
    cmd.setDepthWriteEnable(VK_FALSE);
    cmd.setDepthCompareOp(vk::CompareOp::eAlways);
    cmd.setStencilTestEnable(VK_FALSE);

    // Additive blending via extended dynamic state 3 if available
    if (m_renderer.supportsExtendedDynamicState3()) {
        const auto& caps = m_renderer.extDyn3Caps();
        if (caps.colorBlendEnable) {
            vk::Bool32 enable = VK_TRUE;
            cmd.setColorBlendEnableEXT(0, 1, &enable);
        }
    }

    // Update descriptor to point at this frame's uniform buffer
    vk::DescriptorBufferInfo bufInfo{};
    bufInfo.buffer = frame.uniformBuffer().buffer();
    bufInfo.offset = 0;
    bufInfo.range = sizeof(DeferredLightUBO);

    vk::WriteDescriptorSet write{};
    write.dstSet = m_lightSet;
    write.dstBinding = 0;
    write.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufInfo;
    m_renderer.device().updateDescriptorSets(1, &write, 0, nullptr);

    // Build pipelines lazily
    ShaderModules modules = m_renderer.getShaderModules(SDR_TYPE_DEFERRED_LIGHTING);
    Assertion(modules.vert != nullptr && modules.frag != nullptr, "Deferred lighting shaders not loaded");

    PipelineKey key{};
    key.type = SDR_TYPE_DEFERRED_LIGHTING;
    key.variant_flags = 0;
    key.color_format = m_renderer.getCurrentColorFormat();
    key.depth_format = m_renderer.getDepthFormat();
    key.sample_count = static_cast<VkSampleCountFlagBits>(m_renderer.getSampleCount());
    key.color_attachment_count = m_renderer.getCurrentColorAttachmentCount();
    key.blend_mode = ALPHA_BLEND_ADDITIVE;

    // Fullscreen pipeline: no vertex input
    vertex_layout emptyLayout;
    key.layout_hash = emptyLayout.hash();
    vk::Pipeline fullscreenPipeline = m_renderer.getPipelineWithLayout(key, modules, emptyLayout, m_pipelineLayout.get());

    // Volume pipeline: position-only layout
    key.layout_hash = m_sphere.layout.hash();
    vk::Pipeline volumePipeline = m_renderer.getPipelineWithLayout(key, modules, m_sphere.layout, m_pipelineLayout.get());

    DeferredLightingSharedState shared{*this, fullscreenPipeline, volumePipeline};

    for (auto& light : lights) {
        std::visit([&](auto& l) { l.record(cmd, frame, shared); }, light);
    }
}

} // namespace graphics::vulkan
