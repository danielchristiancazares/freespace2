#pragma once

#include "graphics/MovieTypes.h"
#include "globalincs/pstypes.h"

#include "VulkanDeferredRelease.h"
#include "VulkanPhaseContexts.h"

#include <array>
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

class VulkanDevice;
class VulkanShaderManager;

class VulkanMovieManager {
  public:
  VulkanMovieManager(VulkanDevice& device, VulkanShaderManager& shaders);

  bool initialize(uint32_t maxMovieTextures);
  bool isAvailable() const { return m_available; }

  MovieTextureHandle createMovieTexture(uint32_t width,
	uint32_t height,
	MovieColorSpace colorspace,
	MovieColorRange range);

  void uploadMovieFrame(const UploadCtx& ctx,
	MovieTextureHandle handle,
	const ubyte* y,
	int yStride,
	const ubyte* u,
	int uStride,
	const ubyte* v,
	int vStride);

  void drawMovieTexture(const RenderCtx& ctx,
	MovieTextureHandle handle,
	float x1,
	float y1,
	float x2,
	float y2,
	float alpha);

  void releaseMovieTexture(MovieTextureHandle handle);

  void collect(uint64_t completedSerial) { m_deferredReleases.collect(completedSerial); }
  void setSafeRetireSerial(uint64_t serial) { m_safeRetireSerial = serial; }

  private:
  struct YcbcrConfig {
	vk::UniqueSamplerYcbcrConversion conversion;
	vk::UniqueSampler sampler;
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniquePipeline pipeline;
  };

  struct MovieTexture {
	vk::UniqueImage image;
	vk::UniqueDeviceMemory memory;
	vk::UniqueImageView imageView;

	vk::DescriptorSet descriptorSet = VK_NULL_HANDLE; // sampler is immutable

	uint32_t uploadYStride = 0;
	uint32_t uploadUVStride = 0;
	vk::DeviceSize yOffset = 0;
	vk::DeviceSize uOffset = 0;
	vk::DeviceSize vOffset = 0;
	vk::DeviceSize stagingFrameSize = 0;

	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t ycbcrConfigIndex = 0;
	vk::ImageLayout currentLayout = vk::ImageLayout::eUndefined;
	uint64_t lastUsedSerial = 0;
  };

  static constexpr uint32_t MOVIE_YCBCR_CONFIG_COUNT = 4;

  bool queryFormatSupport();
  void createMovieYcbcrConfigs();
  void createMovieDescriptorPool(uint32_t maxMovieTextures);
  void createMoviePipelines();

  void initMovieStagingLayout(MovieTexture& tex);
  void transitionForUpload(vk::CommandBuffer cmd, MovieTexture& tex);
  void transitionForSampling(vk::CommandBuffer cmd, MovieTexture& tex);

  MovieTexture& getMovieTexture(MovieTextureHandle handle);
  MovieTextureHandle storeMovieTexture(MovieTexture&& tex);
  void freeMovieHandle(MovieTextureHandle handle);

  uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const;
  uint32_t ycbcrIndex(MovieColorSpace colorspace, MovieColorRange range) const;

  VulkanDevice& m_vulkanDevice;
  VulkanShaderManager& m_shaders;
  vk::Device m_device;
  vk::PhysicalDevice m_physicalDevice;
  vk::PhysicalDeviceMemoryProperties m_memoryProperties;
  vk::PipelineCache m_pipelineCache;
  vk::Format m_swapchainFormat = vk::Format::eUndefined;

  bool m_available = false;
  uint32_t m_movieCombinedImageSamplerDescriptorCount = 1;
  vk::ChromaLocation m_chromaLocation = vk::ChromaLocation::eMidpoint;
  vk::Filter m_movieChromaFilter = vk::Filter::eLinear;

  vk::UniqueDescriptorPool m_movieDescriptorPool;
  std::array<YcbcrConfig, MOVIE_YCBCR_CONFIG_COUNT> m_ycbcrConfigs{};

  SCP_unordered_map<uint32_t, MovieTexture> m_movieResident;
  SCP_vector<uint32_t> m_movieFreeHandles;
  DeferredReleaseQueue m_deferredReleases;
  uint64_t m_safeRetireSerial = 0;

  bool m_loggedUnavailable = false;
  bool m_loggedOddDimensions = false;
  bool m_loggedDescriptorAllocFailure = false;
  bool m_loggedStagingAllocFailure = false;
  bool m_loggedTargetFormatMismatch = false;
};

} // namespace vulkan
} // namespace graphics
