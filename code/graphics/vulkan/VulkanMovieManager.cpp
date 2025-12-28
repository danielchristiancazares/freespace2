#include "VulkanMovieManager.h"

#include "VulkanClip.h"
#include "VulkanDebug.h"
#include "VulkanDevice.h"
#include "VulkanFrame.h"
#include "VulkanShaderManager.h"
#include "VulkanSync2Helpers.h"

#include "graphics/2d.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace graphics {
namespace vulkan {

namespace {
struct MoviePushConstants {
  float screenSize[2];
  float rectMin[2];
  float rectMax[2];
  float alpha;
  float pad;
};

static_assert(sizeof(MoviePushConstants) == 32, "MoviePushConstants must be 32 bytes");
static_assert(offsetof(MoviePushConstants, alpha) == 24, "MoviePushConstants.alpha offset mismatch");

static uint32_t alignUpU32(uint32_t v, uint32_t a) { return (v + (a - 1u)) & ~(a - 1u); }
static vk::DeviceSize alignUpSize(vk::DeviceSize v, vk::DeviceSize a) { return (v + (a - 1)) & ~(a - 1); }

void copyPlanePacked(ubyte *dst, uint32_t dstStride, const ubyte *src, int srcStride, uint32_t copyWidth,
                     uint32_t copyHeight) {
  if (srcStride < 0) {
    src += (copyHeight - 1u) * static_cast<uint32_t>(-srcStride);
    srcStride = -srcStride;
  }

  for (uint32_t y = 0; y < copyHeight; ++y) {
    std::memcpy(dst + static_cast<size_t>(dstStride) * y, src + static_cast<size_t>(srcStride) * y, copyWidth);
  }
}

vk::Viewport createMovieViewport() {
  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(gr_screen.max_h);
  viewport.width = static_cast<float>(gr_screen.max_w);
  viewport.height = -static_cast<float>(gr_screen.max_h);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  return viewport;
}

vk::Rect2D createMovieScissor() {
  auto clip = getClipScissorFromScreen(gr_screen);
  clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
  vk::Rect2D scissor{};
  scissor.offset = vk::Offset2D{clip.x, clip.y};
  scissor.extent = vk::Extent2D{clip.width, clip.height};
  return scissor;
}
} // namespace

VulkanMovieManager::VulkanMovieManager(VulkanDevice &device, VulkanShaderManager &shaders)
    : m_vulkanDevice(device), m_shaders(shaders), m_device(device.device()), m_physicalDevice(device.physicalDevice()),
      m_memoryProperties(device.memoryProperties()), m_pipelineCache(device.pipelineCache()),
      m_swapchainFormat(device.swapchainFormat()) {}

bool VulkanMovieManager::initialize(uint32_t maxMovieTextures) {
  if (!m_vulkanDevice.features11().samplerYcbcrConversion) {
    vkprintf("VulkanMovieManager: samplerYcbcrConversion not supported; Vulkan movie path disabled.\n");
    m_available = false;
    return false;
  }

  if (!queryFormatSupport()) {
    m_available = false;
    return false;
  }

  try {
    createMovieYcbcrConfigs();
    createMovieDescriptorPool(maxMovieTextures);
    createMoviePipelines();
  } catch (const std::exception &e) {
    vkprintf("VulkanMovieManager: initialization failed: %s\n", e.what());
    m_available = false;
    return false;
  }

  m_available = true;
  return true;
}

bool VulkanMovieManager::queryFormatSupport() {
  const vk::Format format = vk::Format::eG8B8R83Plane420Unorm;

  vk::FormatProperties2 formatProps{};
  m_physicalDevice.getFormatProperties2(format, &formatProps);

  const auto features = formatProps.formatProperties.optimalTilingFeatures;
  const auto required = vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferDst;
  if ((features & required) != required) {
    vkprintf("VulkanMovieManager: multi-planar format missing sampled/transfer support; disabled.\n");
    return false;
  }

  if (features & vk::FormatFeatureFlagBits::eMidpointChromaSamples) {
    m_chromaLocation = vk::ChromaLocation::eMidpoint;
  } else if (features & vk::FormatFeatureFlagBits::eCositedChromaSamples) {
    m_chromaLocation = vk::ChromaLocation::eCositedEven;
  } else {
    m_chromaLocation = vk::ChromaLocation::eMidpoint;
  }

  if (features & vk::FormatFeatureFlagBits::eSampledImageYcbcrConversionLinearFilter) {
    m_movieChromaFilter = vk::Filter::eLinear;
  } else {
    m_movieChromaFilter = vk::Filter::eNearest;
  }

  vk::PhysicalDeviceImageFormatInfo2 fmtInfo{};
  fmtInfo.format = format;
  fmtInfo.type = vk::ImageType::e2D;
  fmtInfo.tiling = vk::ImageTiling::eOptimal;
  fmtInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
  fmtInfo.flags = {};

  vk::SamplerYcbcrConversionImageFormatProperties ycbcrProps{};
  vk::ImageFormatProperties2 outProps{};
  outProps.pNext = &ycbcrProps;

  auto result = m_physicalDevice.getImageFormatProperties2(&fmtInfo, &outProps);
  if (result != vk::Result::eSuccess) {
    vkprintf("VulkanMovieManager: image format properties query failed; disabled.\n");
    return false;
  }

  m_movieCombinedImageSamplerDescriptorCount = std::max(1u, ycbcrProps.combinedImageSamplerDescriptorCount);
  return true;
}

void VulkanMovieManager::createMovieYcbcrConfigs() {
  for (uint32_t i = 0; i < MOVIE_YCBCR_CONFIG_COUNT; ++i) {
    const auto colorspace = static_cast<MovieColorSpace>(i / 2u);
    const auto range = static_cast<MovieColorRange>(i % 2u);

    vk::SamplerYcbcrConversionCreateInfo convInfo{};
    convInfo.format = vk::Format::eG8B8R83Plane420Unorm;
    convInfo.ycbcrModel = (colorspace == MovieColorSpace::BT709) ? vk::SamplerYcbcrModelConversion::eYcbcr709
                                                                 : vk::SamplerYcbcrModelConversion::eYcbcr601;
    convInfo.ycbcrRange =
        (range == MovieColorRange::Full) ? vk::SamplerYcbcrRange::eItuFull : vk::SamplerYcbcrRange::eItuNarrow;
    convInfo.components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity};
    convInfo.xChromaOffset = m_chromaLocation;
    convInfo.yChromaOffset = m_chromaLocation;
    convInfo.chromaFilter = m_movieChromaFilter;
    convInfo.forceExplicitReconstruction = VK_FALSE;

    auto &cfg = m_ycbcrConfigs[i];
    cfg.conversion = m_device.createSamplerYcbcrConversionUnique(convInfo);

    vk::SamplerYcbcrConversionInfo samplerConvInfo{};
    samplerConvInfo.conversion = cfg.conversion.get();

    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.pNext = &samplerConvInfo;
    samplerInfo.magFilter = m_movieChromaFilter;
    samplerInfo.minFilter = m_movieChromaFilter;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    cfg.sampler = m_device.createSamplerUnique(samplerInfo);

    vk::DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    binding.descriptorCount = 1;
    binding.stageFlags = vk::ShaderStageFlagBits::eFragment;
    const vk::Sampler immutableSampler = cfg.sampler.get();
    binding.pImmutableSamplers = &immutableSampler;

    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    cfg.setLayout = m_device.createDescriptorSetLayoutUnique(layoutInfo);

    vk::PushConstantRange pushRange{};
    pushRange.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    pushRange.offset = 0;
    pushRange.size = sizeof(MoviePushConstants);

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = 1;
    const vk::DescriptorSetLayout setLayout = cfg.setLayout.get();
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    cfg.pipelineLayout = m_device.createPipelineLayoutUnique(pipelineLayoutInfo);
  }
}

void VulkanMovieManager::createMovieDescriptorPool(uint32_t maxMovieTextures) {
  vk::DescriptorPoolSize poolSize{};
  poolSize.type = vk::DescriptorType::eCombinedImageSampler;
  poolSize.descriptorCount = maxMovieTextures * m_movieCombinedImageSamplerDescriptorCount;

  vk::DescriptorPoolCreateInfo poolInfo{};
  poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
  poolInfo.maxSets = maxMovieTextures;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;

  m_movieDescriptorPool = m_device.createDescriptorPoolUnique(poolInfo);
}

void VulkanMovieManager::createMoviePipelines() {
  ShaderModules modules = m_shaders.getModulesByFilenames("movie.vert.spv", "movie.frag.spv");
  if (!modules.vert || !modules.frag) {
    vkprintf("VulkanMovieManager: missing movie shader modules (movie.vert.spv/movie.frag.spv).\n");
    throw std::runtime_error("Missing movie shader modules");
  }
  Assertion(modules.vert, "VulkanMovieManager: missing movie.vert.spv shader module");
  Assertion(modules.frag, "VulkanMovieManager: missing movie.frag.spv shader module");

  std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
  stages[0].stage = vk::ShaderStageFlagBits::eVertex;
  stages[0].module = modules.vert;
  stages[0].pName = "main";
  stages[1].stage = vk::ShaderStageFlagBits::eFragment;
  stages[1].module = modules.frag;
  stages[1].pName = "main";

  vk::PipelineVertexInputStateCreateInfo vi{};

  vk::PipelineInputAssemblyStateCreateInfo ia{};
  ia.topology = vk::PrimitiveTopology::eTriangleList;
  ia.primitiveRestartEnable = VK_FALSE;

  vk::PipelineViewportStateCreateInfo vp{};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  vk::PipelineRasterizationStateCreateInfo rs{};
  rs.polygonMode = vk::PolygonMode::eFill;
  rs.cullMode = vk::CullModeFlagBits::eNone;
  rs.frontFace = vk::FrontFace::eClockwise;
  rs.lineWidth = 1.0f;

  vk::PipelineMultisampleStateCreateInfo ms{};
  ms.rasterizationSamples = vk::SampleCountFlagBits::e1;

  vk::PipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
  blend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
  blend.colorBlendOp = vk::BlendOp::eAdd;
  blend.srcAlphaBlendFactor = vk::BlendFactor::eOne;
  blend.dstAlphaBlendFactor = vk::BlendFactor::eZero;
  blend.alphaBlendOp = vk::BlendOp::eAdd;
  blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

  vk::PipelineColorBlendStateCreateInfo cb{};
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;

  vk::PipelineDepthStencilStateCreateInfo ds{};
  ds.depthTestEnable = VK_FALSE;
  ds.depthWriteEnable = VK_FALSE;
  ds.depthCompareOp = vk::CompareOp::eAlways;
  ds.stencilTestEnable = VK_FALSE;

  std::array<vk::DynamicState, 3> dynStates = {
      vk::DynamicState::eViewport,
      vk::DynamicState::eScissor,
      vk::DynamicState::ePrimitiveTopology,
  };
  vk::PipelineDynamicStateCreateInfo dyn{};
  dyn.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
  dyn.pDynamicStates = dynStates.data();

  vk::PipelineRenderingCreateInfo renderingInfo{};
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachmentFormats = &m_swapchainFormat;
  renderingInfo.depthAttachmentFormat = vk::Format::eUndefined;
  renderingInfo.stencilAttachmentFormat = vk::Format::eUndefined;

  for (auto &cfg : m_ycbcrConfigs) {
    vk::GraphicsPipelineCreateInfo info{};
    info.pNext = &renderingInfo;
    info.stageCount = static_cast<uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &cb;
    info.pDynamicState = &dyn;
    info.layout = cfg.pipelineLayout.get();
    info.renderPass = VK_NULL_HANDLE;

    auto pipelineResult = m_device.createGraphicsPipelineUnique(m_pipelineCache, info);
    if (pipelineResult.result != vk::Result::eSuccess) {
      throw std::runtime_error("Failed to create Vulkan movie pipeline.");
    }
    cfg.pipeline = std::move(pipelineResult.value);
  }
}

MovieTextureHandle VulkanMovieManager::createMovieTexture(uint32_t width, uint32_t height, MovieColorSpace colorspace,
                                                          MovieColorRange range) {
  if (!m_available) {
    if (!m_loggedUnavailable) {
      vkprintf("VulkanMovieManager: createMovieTexture rejected; movie path unavailable.\n");
      m_loggedUnavailable = true;
    }
    return MovieTextureHandle::Invalid;
  }

  if ((width & 1u) != 0u || (height & 1u) != 0u) {
    if (!m_loggedOddDimensions) {
      vkprintf("VulkanMovieManager: YUV420 requires even dimensions; got %ux%u.\n", width, height);
      m_loggedOddDimensions = true;
    }
    return MovieTextureHandle::Invalid;
  }

  try {
    MovieTexture tex{};
    tex.width = width;
    tex.height = height;
    tex.ycbcrConfigIndex = ycbcrIndex(colorspace, range);
    const auto &cfg = m_ycbcrConfigs[tex.ycbcrConfigIndex];

    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.format = vk::Format::eG8B8R83Plane420Unorm;
    imageInfo.extent = vk::Extent3D{width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;
    tex.image = m_device.createImageUnique(imageInfo);

    vk::ImageMemoryRequirementsInfo2 reqInfo{};
    reqInfo.image = tex.image.get();

    vk::MemoryDedicatedRequirements dedicatedReqs{};
    vk::MemoryRequirements2 req2{};
    req2.pNext = &dedicatedReqs;
    m_device.getImageMemoryRequirements2(&reqInfo, &req2);

    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = req2.memoryRequirements.size;
    allocInfo.memoryTypeIndex =
        findMemoryType(req2.memoryRequirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

    vk::MemoryDedicatedAllocateInfo dedicatedAlloc{};
    if (dedicatedReqs.requiresDedicatedAllocation || dedicatedReqs.prefersDedicatedAllocation) {
      dedicatedAlloc.image = tex.image.get();
      allocInfo.pNext = &dedicatedAlloc;
    }

    tex.memory = m_device.allocateMemoryUnique(allocInfo);
    m_device.bindImageMemory(tex.image.get(), tex.memory.get(), 0);

    vk::SamplerYcbcrConversionInfo viewConvInfo{};
    viewConvInfo.conversion = cfg.conversion.get();

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.pNext = &viewConvInfo;
    viewInfo.image = tex.image.get();
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = vk::Format::eG8B8R83Plane420Unorm;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    tex.imageView = m_device.createImageViewUnique(viewInfo);

    vk::DescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.descriptorPool = m_movieDescriptorPool.get();
    setAllocInfo.descriptorSetCount = 1;
    const vk::DescriptorSetLayout setLayout = cfg.setLayout.get();
    setAllocInfo.pSetLayouts = &setLayout;

    auto sets = m_device.allocateDescriptorSets(setAllocInfo);
    if (sets.empty()) {
      if (!m_loggedDescriptorAllocFailure) {
        vkprintf("VulkanMovieManager: descriptor set allocation failed for movie texture.\n");
        m_loggedDescriptorAllocFailure = true;
      }
      return MovieTextureHandle::Invalid;
    }
    tex.descriptorSet = sets[0];

    vk::DescriptorImageInfo imageDescInfo{};
    imageDescInfo.imageView = tex.imageView.get();
    imageDescInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    vk::WriteDescriptorSet write{};
    write.dstSet = tex.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.pImageInfo = &imageDescInfo;
    m_device.updateDescriptorSets(write, {});

    initMovieStagingLayout(tex);

    return storeMovieTexture(std::move(tex));
  } catch (const std::exception &e) {
    vkprintf("VulkanMovieManager: failed to create movie texture (%ux%u): %s\n", width, height, e.what());
    return MovieTextureHandle::Invalid;
  }
}

void VulkanMovieManager::uploadMovieFrame(const UploadCtx &ctx, MovieTextureHandle handle, const ubyte *y, int yStride,
                                          const ubyte *u, int uStride, const ubyte *v, int vStride) {
  if (!m_available || !gr_is_valid(handle)) {
    return;
  }

  MovieTexture &tex = getMovieTexture(handle);
  vk::CommandBuffer cmd = ctx.cmd;

  auto allocOpt = ctx.frame.stagingBuffer().try_allocate(tex.stagingFrameSize, 4);
  if (!allocOpt) {
    if (!m_loggedStagingAllocFailure) {
      vkprintf("VulkanMovieManager: staging allocation failed (%llu bytes) for movie upload; frame dropped.\n",
               static_cast<unsigned long long>(tex.stagingFrameSize));
      m_loggedStagingAllocFailure = true;
    }
    return;
  }
  const auto alloc = *allocOpt;
  auto *base = static_cast<ubyte *>(alloc.mapped);

  const uint32_t uvW = tex.width / 2;
  const uint32_t uvH = tex.height / 2;

  copyPlanePacked(base + tex.yOffset, tex.uploadYStride, y, yStride, tex.width, tex.height);
  copyPlanePacked(base + tex.uOffset, tex.uploadUVStride, u, uStride, uvW, uvH);
  copyPlanePacked(base + tex.vOffset, tex.uploadUVStride, v, vStride, uvW, uvH);

  transitionForUpload(cmd, tex);

  std::array<vk::BufferImageCopy, 3> copies{};

  copies[0].bufferOffset = alloc.offset + tex.yOffset;
  copies[0].bufferRowLength = tex.uploadYStride;
  copies[0].bufferImageHeight = 0;
  copies[0].imageSubresource.aspectMask = vk::ImageAspectFlagBits::ePlane0;
  copies[0].imageSubresource.mipLevel = 0;
  copies[0].imageSubresource.baseArrayLayer = 0;
  copies[0].imageSubresource.layerCount = 1;
  copies[0].imageExtent = vk::Extent3D{tex.width, tex.height, 1};

  copies[1].bufferOffset = alloc.offset + tex.uOffset;
  copies[1].bufferRowLength = tex.uploadUVStride;
  copies[1].bufferImageHeight = 0;
  copies[1].imageSubresource.aspectMask = vk::ImageAspectFlagBits::ePlane1;
  copies[1].imageSubresource.mipLevel = 0;
  copies[1].imageSubresource.baseArrayLayer = 0;
  copies[1].imageSubresource.layerCount = 1;
  copies[1].imageExtent = vk::Extent3D{uvW, uvH, 1};

  copies[2].bufferOffset = alloc.offset + tex.vOffset;
  copies[2].bufferRowLength = tex.uploadUVStride;
  copies[2].bufferImageHeight = 0;
  copies[2].imageSubresource.aspectMask = vk::ImageAspectFlagBits::ePlane2;
  copies[2].imageSubresource.mipLevel = 0;
  copies[2].imageSubresource.baseArrayLayer = 0;
  copies[2].imageSubresource.layerCount = 1;
  copies[2].imageExtent = vk::Extent3D{uvW, uvH, 1};

  cmd.copyBufferToImage(ctx.frame.stagingBuffer().buffer(), tex.image.get(), vk::ImageLayout::eTransferDstOptimal,
                        copies);

  transitionForSampling(cmd, tex);
}

void VulkanMovieManager::drawMovieTexture(const RenderCtx &ctx, MovieTextureHandle handle, float x1, float y1, float x2,
                                          float y2, float alpha) {
  if (!m_available || !gr_is_valid(handle)) {
    return;
  }

  MovieTexture &tex = getMovieTexture(handle);
  const YcbcrConfig &cfg = m_ycbcrConfigs[tex.ycbcrConfigIndex];
  vk::CommandBuffer cmd = ctx.cmd;

  if (ctx.targetInfo.colorFormat != m_swapchainFormat && !m_loggedTargetFormatMismatch) {
    vkprintf("VulkanMovieManager: movie draw target format (%d) does not match swapchain format (%d).\n",
             static_cast<int>(ctx.targetInfo.colorFormat), static_cast<int>(m_swapchainFormat));
    m_loggedTargetFormatMismatch = true;
  }

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, cfg.pipeline.get());
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, cfg.pipelineLayout.get(), 0, tex.descriptorSet, {});

  MoviePushConstants pc{};
  pc.screenSize[0] = static_cast<float>(gr_screen.max_w);
  pc.screenSize[1] = static_cast<float>(gr_screen.max_h);
  pc.rectMin[0] = x1;
  pc.rectMin[1] = y1;
  pc.rectMax[0] = x2;
  pc.rectMax[1] = y2;
  pc.alpha = alpha;
  pc.pad = 0.0f;

  cmd.pushConstants(cfg.pipelineLayout.get(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
                    sizeof(pc), &pc);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);

  vk::Viewport viewport = createMovieViewport();
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor = createMovieScissor();
  cmd.setScissor(0, 1, &scissor);

  cmd.draw(6, 1, 0, 0);

  tex.lastUsedSerial = m_safeRetireSerial;
}

void VulkanMovieManager::releaseMovieTexture(MovieTextureHandle handle) {
  if (!gr_is_valid(handle)) {
    return;
  }

  const auto key = static_cast<uint32_t>(handle);
  auto it = m_movieResident.find(key);
  if (it == m_movieResident.end()) {
    return;
  }

  MovieTexture tex = std::move(it->second);
  m_movieResident.erase(it);

  const uint64_t retireSerial = std::max(m_safeRetireSerial, tex.lastUsedSerial);
  auto pool = m_movieDescriptorPool.get();
  auto dev = m_device;
  m_deferredReleases.enqueue(retireSerial, [t = std::move(tex), pool, dev]() mutable {
    if (t.descriptorSet) {
      dev.freeDescriptorSets(pool, t.descriptorSet);
      t.descriptorSet = VK_NULL_HANDLE;
    }
    (void)t;
  });

  freeMovieHandle(handle);
}

void VulkanMovieManager::initMovieStagingLayout(MovieTexture &tex) {
  const uint32_t uvW = tex.width / 2;
  const uint32_t uvH = tex.height / 2;

  tex.uploadYStride = alignUpU32(tex.width, 4);
  tex.uploadUVStride = alignUpU32(uvW, 4);

  const vk::DeviceSize ySize = vk::DeviceSize(tex.uploadYStride) * tex.height;
  const vk::DeviceSize uSize = vk::DeviceSize(tex.uploadUVStride) * uvH;
  const vk::DeviceSize vSize = vk::DeviceSize(tex.uploadUVStride) * uvH;

  tex.yOffset = 0;
  tex.uOffset = alignUpSize(tex.yOffset + ySize, 4);
  tex.vOffset = alignUpSize(tex.uOffset + uSize, 4);
  tex.stagingFrameSize = alignUpSize(tex.vOffset + vSize, 4);
}

void VulkanMovieManager::transitionForUpload(vk::CommandBuffer cmd, MovieTexture &tex) {
  const bool fromShaderRead = (tex.currentLayout == vk::ImageLayout::eShaderReadOnlyOptimal);
  auto barrier = makeImageBarrier(
      tex.image.get(),
      fromShaderRead ? vk::PipelineStageFlagBits2::eFragmentShader : vk::PipelineStageFlagBits2::eTopOfPipe,
      fromShaderRead ? vk::AccessFlagBits2::eShaderRead : vk::AccessFlags2{}, vk::PipelineStageFlagBits2::eTransfer,
      vk::AccessFlagBits2::eTransferWrite, tex.currentLayout, vk::ImageLayout::eTransferDstOptimal,
      vk::ImageAspectFlagBits::eColor, 1, 1);
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  submitImageBarrier(cmd, barrier);

  tex.currentLayout = vk::ImageLayout::eTransferDstOptimal;
}

void VulkanMovieManager::transitionForSampling(vk::CommandBuffer cmd, MovieTexture &tex) {
  auto barrier = makeImageBarrier(tex.image.get(), vk::PipelineStageFlagBits2::eTransfer,
                                  vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                                  vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eTransferDstOptimal,
                                  vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor, 1, 1);
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  submitImageBarrier(cmd, barrier);

  tex.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
}

VulkanMovieManager::MovieTexture &VulkanMovieManager::getMovieTexture(MovieTextureHandle handle) {
  Assertion(handle != MovieTextureHandle::Invalid, "Invalid movie texture handle");
  auto it = m_movieResident.find(static_cast<uint32_t>(handle));
  Assertion(it != m_movieResident.end(), "Stale or invalid movie texture handle");
  return it->second;
}

MovieTextureHandle VulkanMovieManager::storeMovieTexture(MovieTexture &&tex) {
  uint32_t idx = 0;
  if (!m_movieFreeHandles.empty()) {
    idx = m_movieFreeHandles.back();
    m_movieFreeHandles.pop_back();
  } else {
    idx = static_cast<uint32_t>(m_movieResident.size());
    while (m_movieResident.find(idx) != m_movieResident.end()) {
      ++idx;
    }
  }
  m_movieResident.emplace(idx, std::move(tex));
  return static_cast<MovieTextureHandle>(idx);
}

void VulkanMovieManager::freeMovieHandle(MovieTextureHandle handle) {
  m_movieFreeHandles.push_back(static_cast<uint32_t>(handle));
}

uint32_t VulkanMovieManager::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const {
  for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i) {
    if ((typeFilter & (1u << i)) && (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }

  Assertion(false, "Failed to find suitable memory type for movie texture");
  return 0;
}

uint32_t VulkanMovieManager::ycbcrIndex(MovieColorSpace colorspace, MovieColorRange range) const {
  return static_cast<uint32_t>(colorspace) * 2u + static_cast<uint32_t>(range);
}

} // namespace vulkan
} // namespace graphics
