#include "VulkanRenderer.h"

#include "VulkanModelValidation.h"
#include "VulkanMovieManager.h"
#include "VulkanTextureBindings.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace graphics {
namespace vulkan {

VulkanRenderer::VulkanRenderer(std::unique_ptr<os::GraphicsOperations> graphicsOps)
    : m_vulkanDevice(std::make_unique<VulkanDevice>(std::move(graphicsOps))) {}

VulkanRenderer::~VulkanRenderer() = default;

bool VulkanRenderer::initialize() {
  if (!m_vulkanDevice->initialize()) {
    return false;
  }

  createDescriptorResources();
  createRenderTargets();
  createUploadCommandPool();
  createSubmitTimelineSemaphore();
  createFrames();

  const SCP_string shaderRoot = "code/graphics/shaders/compiled";
  m_shaderManager = std::make_unique<VulkanShaderManager>(m_vulkanDevice->device(), shaderRoot);

  m_movieManager = std::make_unique<VulkanMovieManager>(*m_vulkanDevice, *m_shaderManager);
  constexpr uint32_t kMaxMovieTextures = 8;
  m_movieManager->initialize(kMaxMovieTextures);

  m_pipelineManager = std::make_unique<VulkanPipelineManager>(
      m_vulkanDevice->device(), m_descriptorLayouts->pipelineLayout(), m_descriptorLayouts->modelPipelineLayout(),
      m_descriptorLayouts->deferredPipelineLayout(), m_vulkanDevice->pipelineCache(),
      m_vulkanDevice->supportsExtendedDynamicState3(), m_vulkanDevice->extDyn3Caps(),
      m_vulkanDevice->supportsVertexAttributeDivisor(), m_vulkanDevice->features13().dynamicRendering == VK_TRUE);

  m_bufferManager =
      std::make_unique<VulkanBufferManager>(m_vulkanDevice->device(), m_vulkanDevice->memoryProperties(),
                                            m_vulkanDevice->graphicsQueue(), m_vulkanDevice->graphicsQueueIndex());

  m_textureManager =
      std::make_unique<VulkanTextureManager>(m_vulkanDevice->device(), m_vulkanDevice->memoryProperties(),
                                             m_vulkanDevice->graphicsQueue(), m_vulkanDevice->graphicsQueueIndex());
  m_textureBindings = std::make_unique<VulkanTextureBindings>(*m_textureManager);
  m_textureUploader = std::make_unique<VulkanTextureUploader>(*m_textureManager);

  // Rendering session depends on the texture manager for bitmap render targets (RTT).
  createRenderingSession();

  createDeferredLightingResources();
  const InitCtx initCtx{};
  createSmaaLookupTextures(initCtx);

  m_inFlightFrames.clear();

  return true;
}

void VulkanRenderer::createDescriptorResources() {
  // Validate device limits before creating layouts - hard assert on failure
  VulkanDescriptorLayouts::validateDeviceLimits(m_vulkanDevice->properties().limits);
  EnsurePushDescriptorSupport(m_vulkanDevice->features14());

  m_descriptorLayouts = std::make_unique<VulkanDescriptorLayouts>(m_vulkanDevice->device());
}

void VulkanRenderer::createFrames() {
  const auto &props = m_vulkanDevice->properties();
  m_availableFrames.clear();
  for (size_t i = 0; i < kFramesInFlight; ++i) {
    vk::DescriptorSet globalSet = m_descriptorLayouts->allocateGlobalSet();
    Assertion(globalSet, "Failed to allocate global descriptor set for frame %zu", i);

    vk::DescriptorSet modelSet = m_descriptorLayouts->allocateModelDescriptorSet();
    Assertion(modelSet, "Failed to allocate model descriptor set for frame %zu", i);

    m_frames[i] = std::make_unique<VulkanFrame>(
        m_vulkanDevice->device(), static_cast<uint32_t>(i), m_vulkanDevice->graphicsQueueIndex(),
        m_vulkanDevice->memoryProperties(), UNIFORM_RING_SIZE, props.limits.minUniformBufferOffsetAlignment,
        VERTEX_RING_SIZE, m_vulkanDevice->vertexBufferAlignment(), STAGING_RING_SIZE,
        props.limits.optimalBufferCopyOffsetAlignment, globalSet, modelSet);

    // Newly created frames haven't been submitted yet; completedSerial is whatever we last observed.
    m_availableFrames.push_back(AvailableFrame{m_frames[i].get(), m_completedSerial});
  }
}

void VulkanRenderer::createRenderTargets() {
  m_renderTargets = std::make_unique<VulkanRenderTargets>(*m_vulkanDevice);
  m_renderTargets->create(m_vulkanDevice->swapchainExtent());
}

void VulkanRenderer::createRenderingSession() {
  Assertion(m_textureManager != nullptr, "createRenderingSession requires a valid texture manager");
  m_renderingSession = std::make_unique<VulkanRenderingSession>(*m_vulkanDevice, *m_renderTargets, *m_textureManager);
}

void VulkanRenderer::createUploadCommandPool() {
  vk::CommandPoolCreateInfo poolInfo;
  poolInfo.queueFamilyIndex = m_vulkanDevice->graphicsQueueIndex();
  poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient;
  m_uploadCommandPool = m_vulkanDevice->device().createCommandPoolUnique(poolInfo);
}

void VulkanRenderer::createSubmitTimelineSemaphore() {
  vk::SemaphoreTypeCreateInfo timelineType;
  timelineType.semaphoreType = vk::SemaphoreType::eTimeline;
  timelineType.initialValue = 0;

  vk::SemaphoreCreateInfo semaphoreInfo;
  semaphoreInfo.pNext = &timelineType;
  m_submitTimeline = m_vulkanDevice->device().createSemaphoreUnique(semaphoreInfo);
  Assertion(m_submitTimeline, "Failed to create submit timeline semaphore");
}

void VulkanRenderer::submitInitCommandsAndWait(const InitCtx & /*init*/,
                                               const std::function<void(vk::CommandBuffer)> &recorder) {
  Assertion(m_vulkanDevice != nullptr, "submitInitCommandsAndWait requires VulkanDevice");
  Assertion(m_uploadCommandPool, "submitInitCommandsAndWait requires an upload command pool");
  Assertion(m_submitTimeline, "submitInitCommandsAndWait requires a submit timeline semaphore");

  vk::CommandBufferAllocateInfo allocInfo{};
  allocInfo.level = vk::CommandBufferLevel::ePrimary;
  allocInfo.commandPool = m_uploadCommandPool.get();
  allocInfo.commandBufferCount = 1;

  vk::CommandBuffer cmd{};
#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  const auto allocRv = m_vulkanDevice->device().allocateCommandBuffers(allocInfo);
  if (allocRv.result != vk::Result::eSuccess || allocRv.value.empty()) {
    throw std::runtime_error("Failed to allocate init command buffer");
  }
  cmd = allocRv.value[0];
#else
  const auto cmdBuffers = m_vulkanDevice->device().allocateCommandBuffers(allocInfo);
  Assertion(!cmdBuffers.empty(), "Failed to allocate init command buffer");
  cmd = cmdBuffers[0];
#endif

  vk::CommandBufferBeginInfo beginInfo{};
  beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  const auto beginResult = cmd.begin(beginInfo);
  if (beginResult != vk::Result::eSuccess) {
    throw std::runtime_error("Failed to begin init command buffer");
  }
#else
  cmd.begin(beginInfo);
#endif

  recorder(cmd);

#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  const auto endResult = cmd.end();
  if (endResult != vk::Result::eSuccess) {
    throw std::runtime_error("Failed to end init command buffer");
  }
#else
  cmd.end();
#endif

  // Integrate with the renderer's global serial model by signaling the timeline semaphore.
  const uint64_t submitSerial = ++m_submitSerial;

  vk::CommandBufferSubmitInfo cmdInfo{};
  cmdInfo.commandBuffer = cmd;

  vk::SemaphoreSubmitInfo signal{};
  signal.semaphore = m_submitTimeline.get();
  signal.value = submitSerial;
  signal.stageMask = vk::PipelineStageFlagBits2::eAllCommands;

  vk::SubmitInfo2 submitInfo{};
  submitInfo.commandBufferInfoCount = 1;
  submitInfo.pCommandBufferInfos = &cmdInfo;
  submitInfo.signalSemaphoreInfoCount = 1;
  submitInfo.pSignalSemaphoreInfos = &signal;

  m_vulkanDevice->graphicsQueue().submit2(submitInfo, nullptr);

  // Block until the submitted work is complete. This avoids the global stall of queue.waitIdle().
  vk::Semaphore semaphore = m_submitTimeline.get();
  vk::SemaphoreWaitInfo waitInfo{};
  waitInfo.semaphoreCount = 1;
  waitInfo.pSemaphores = &semaphore;
  waitInfo.pValues = &submitSerial;

#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  const auto waitResult = m_vulkanDevice->device().waitSemaphores(waitInfo, std::numeric_limits<uint64_t>::max());
  if (waitResult != vk::Result::eSuccess) {
    throw std::runtime_error("Failed waiting for init submit to complete");
  }
#else
  m_vulkanDevice->device().waitSemaphores(waitInfo, std::numeric_limits<uint64_t>::max());
#endif

  m_completedSerial = std::max(m_completedSerial, submitSerial);

  // Safe after wait: recycle command buffer allocations from the init command pool.
#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  const auto resetResult = m_vulkanDevice->device().resetCommandPool(m_uploadCommandPool.get());
  if (resetResult != vk::Result::eSuccess) {
    throw std::runtime_error("Failed to reset init upload command pool");
  }
#else
  m_vulkanDevice->device().resetCommandPool(m_uploadCommandPool.get());
#endif
}

void VulkanRenderer::shutdown() {
  if (!m_vulkanDevice) {
    return; // Already shut down or never initialized
  }

  m_vulkanDevice->device().waitIdle();

  // Clear non-owned handles
  // All RAII members are cleaned up by destructors in reverse declaration order

  // VulkanDevice shutdown is handled by its destructor
}

void VulkanRenderer::createDeferredLightingResources() {
  // Fullscreen triangle (covers entire screen with 3 vertices, no clipping)
  // Positions are in clip space: vertex shader passes through directly
  struct FullscreenVertex {
    float x, y, z;
  };
  static const FullscreenVertex fullscreenVerts[] = {{-1.0f, -1.0f, 0.0f}, {3.0f, -1.0f, 0.0f}, {-1.0f, 3.0f, 0.0f}};

  m_fullscreenMesh.vbo = m_bufferManager->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_fullscreenMesh.vbo, sizeof(fullscreenVerts), fullscreenVerts);
  m_fullscreenMesh.indexCount = 3;

  // Sphere mesh (icosphere-like approximation)
  // Using octahedron subdivided once for reasonable approximation
  std::vector<float> sphereVerts;
  std::vector<uint32_t> sphereIndices;

  // Octahedron base vertices
  const float oct[] = {
      0.0f,  1.0f,  0.0f, // top
      0.0f,  -1.0f, 0.0f, // bottom
      1.0f,  0.0f,  0.0f, // +X
      -1.0f, 0.0f,  0.0f, // -X
      0.0f,  0.0f,  1.0f, // +Z
      0.0f,  0.0f,  -1.0f // -Z
  };

  // Octahedron faces (8 triangles)
  const uint32_t octFaces[] = {
      0, 4, 2, 0, 2, 5, 0, 5, 3, 0, 3, 4, // top half
      1, 2, 4, 1, 5, 2, 1, 3, 5, 1, 4, 3  // bottom half
  };

  for (int i = 0; i < 18; i++) {
    sphereVerts.push_back(oct[i]);
  }
  for (int i = 0; i < 24; i++) {
    sphereIndices.push_back(octFaces[i]);
  }

  m_sphereMesh.vbo = m_bufferManager->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_sphereMesh.vbo, sphereVerts.size() * sizeof(float), sphereVerts.data());
  m_sphereMesh.ibo = m_bufferManager->createBuffer(BufferType::Index, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_sphereMesh.ibo, sphereIndices.size() * sizeof(uint32_t), sphereIndices.data());
  m_sphereMesh.indexCount = static_cast<uint32_t>(sphereIndices.size());

  // Cylinder mesh (capped cylinder along -Z axis)
  // The model matrix will position and scale it
  std::vector<float> cylVerts;
  std::vector<uint32_t> cylIndices;

  const int segments = 12;
  const float twoPi = 6.283185307f;

  // Generate ring vertices at z=0 and z=-1
  for (int ring = 0; ring < 2; ++ring) {
    float z = (ring == 0) ? 0.0f : -1.0f;
    for (int i = 0; i < segments; ++i) {
      float angle = twoPi * i / segments;
      cylVerts.push_back(cosf(angle)); // x
      cylVerts.push_back(sinf(angle)); // y
      cylVerts.push_back(z);           // z
    }
  }

  // Center vertices for caps
  uint32_t capTop = static_cast<uint32_t>(cylVerts.size() / 3);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(0.0f);

  uint32_t capBot = static_cast<uint32_t>(cylVerts.size() / 3);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(-1.0f);

  // Side faces (quads as two triangles)
  for (int i = 0; i < segments; ++i) {
    uint32_t i0 = i;
    uint32_t i1 = (i + 1) % segments;
    uint32_t i2 = i + segments;
    uint32_t i3 = ((i + 1) % segments) + segments;

    // Two triangles per quad
    cylIndices.push_back(i0);
    cylIndices.push_back(i2);
    cylIndices.push_back(i1);

    cylIndices.push_back(i1);
    cylIndices.push_back(i2);
    cylIndices.push_back(i3);
  }

  // Top cap (z=0)
  for (int i = 0; i < segments; ++i) {
    cylIndices.push_back(capTop);
    cylIndices.push_back((i + 1) % segments);
    cylIndices.push_back(i);
  }

  // Bottom cap (z=-1)
  for (int i = 0; i < segments; ++i) {
    cylIndices.push_back(capBot);
    cylIndices.push_back(i + segments);
    cylIndices.push_back(((i + 1) % segments) + segments);
  }

  m_cylinderMesh.vbo = m_bufferManager->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_cylinderMesh.vbo, cylVerts.size() * sizeof(float), cylVerts.data());
  m_cylinderMesh.ibo = m_bufferManager->createBuffer(BufferType::Index, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_cylinderMesh.ibo, cylIndices.size() * sizeof(uint32_t), cylIndices.data());
  m_cylinderMesh.indexCount = static_cast<uint32_t>(cylIndices.size());
}

} // namespace vulkan
} // namespace graphics
