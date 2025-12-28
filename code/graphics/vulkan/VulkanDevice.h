#pragma once

#include "VulkanDebug.h"
#include "osapi/osapi.h"

#include <cstdint>
#include <memory>
#include <vulkan/vulkan.hpp>

#if SDL_VERSION_ATLEAST(2, 0, 6)
#define SDL_SUPPORTS_VULKAN 1
#else
#define SDL_SUPPORTS_VULKAN 0
#endif

namespace graphics {
namespace vulkan {

struct QueueIndex {
  bool initialized = false;
  uint32_t index = 0;
};

struct PhysicalDeviceValues {
  vk::PhysicalDevice device;
  vk::PhysicalDeviceProperties properties;
  vk::PhysicalDeviceFeatures features;
  vk::PhysicalDeviceVulkan11Features features11;
  vk::PhysicalDeviceVulkan12Features features12;
  vk::PhysicalDeviceVulkan13Features features13;
  vk::PhysicalDeviceVulkan14Features features14;
  vk::PhysicalDevicePushDescriptorPropertiesKHR pushDescriptorProps{};
  vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT extDynamicState{};
  vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT extDynamicState2{};
  vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT extDynamicState3{};

  std::vector<vk::ExtensionProperties> extensions;

  vk::SurfaceCapabilitiesKHR surfaceCapabilities;
  std::vector<vk::SurfaceFormatKHR> surfaceFormats;
  std::vector<vk::PresentModeKHR> presentModes;

  std::vector<vk::QueueFamilyProperties> queueProperties;
  QueueIndex graphicsQueueIndex;
  QueueIndex presentQueueIndex;
};

// Device scoring for GPU selection (exposed for testing)
uint32_t deviceTypeScore(vk::PhysicalDeviceType type);
uint32_t scoreDevice(const PhysicalDeviceValues &device);

class VulkanDevice {
public:
  explicit VulkanDevice(std::unique_ptr<os::GraphicsOperations> graphicsOps);
  ~VulkanDevice();

  VulkanDevice(const VulkanDevice &) = delete;
  VulkanDevice &operator=(const VulkanDevice &) = delete;
  VulkanDevice(VulkanDevice &&) = delete;
  VulkanDevice &operator=(VulkanDevice &&) = delete;

  bool initialize();
  void shutdown();

  //--------------------------------------------------------------------------
  // Presentation lifecycle
  //--------------------------------------------------------------------------
  struct AcquireResult {
    uint32_t imageIndex = 0;
    bool needsRecreate = false; // VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR
    bool success = false;
  };
  AcquireResult acquireNextImage(vk::Semaphore imageAvailable);

  struct PresentResult {
    bool needsRecreate = false;
    bool success = false;
  };
  PresentResult present(vk::Semaphore renderFinished, uint32_t imageIndex);

  bool recreateSwapchain(uint32_t width, uint32_t height);

  //--------------------------------------------------------------------------
  // Core Vulkan handles (read-only access)
  //--------------------------------------------------------------------------
  vk::Instance instance() const { return m_instance.get(); }
  vk::PhysicalDevice physicalDevice() const { return m_physicalDevice; }
  vk::Device device() const { return m_device.get(); }
  vk::Queue graphicsQueue() const { return m_graphicsQueue; }
  vk::Queue presentQueue() const { return m_presentQueue; }
  uint32_t graphicsQueueIndex() const { return m_graphicsQueueIndex; }
  uint32_t presentQueueIndex() const { return m_presentQueueIndex; }

  //--------------------------------------------------------------------------
  // Swapchain access
  //--------------------------------------------------------------------------
  vk::SwapchainKHR swapchain() const { return m_swapchain.get(); }
  vk::Format swapchainFormat() const { return m_swapchainFormat; }
  vk::Extent2D swapchainExtent() const { return m_swapchainExtent; }
  vk::Image swapchainImage(uint32_t index) const;
  vk::ImageView swapchainImageView(uint32_t index) const;
  uint32_t swapchainImageCount() const;
  // Render-finished semaphore to use for presenting a specific swapchain image index.
  // Indexed by the acquired swapchain image index to avoid reusing a present semaphore before reacquire.
  vk::Semaphore swapchainRenderFinishedSemaphore(uint32_t imageIndex) const;
  vk::ImageUsageFlags swapchainUsage() const { return m_swapchainUsage; }
  uint64_t swapchainGeneration() const { return m_swapchainGeneration; }

  //--------------------------------------------------------------------------
  // Device properties and capabilities
  //--------------------------------------------------------------------------
  const vk::PhysicalDeviceProperties &properties() const { return m_properties; }
  const vk::PhysicalDeviceMemoryProperties &memoryProperties() const { return m_memoryProperties; }
  const vk::PhysicalDeviceVulkan11Features &features11() const { return m_features11; }
  const vk::PhysicalDeviceVulkan13Features &features13() const { return m_features13; }
  const vk::PhysicalDeviceVulkan14Features &features14() const { return m_features14; }
  const ExtendedDynamicState3Caps &extDyn3Caps() const { return m_extDyn3Caps; }
  bool supportsExtendedDynamicState() const { return m_supportsExtDyn; }
  bool supportsExtendedDynamicState2() const { return m_supportsExtDyn2; }
  bool supportsExtendedDynamicState3() const { return m_supportsExtDyn3; }
  bool supportsVertexAttributeDivisor() const { return m_supportsVertexAttributeDivisor; }

  //--------------------------------------------------------------------------
  // Utilities
  //--------------------------------------------------------------------------
  uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags props) const;
  size_t minUniformBufferOffsetAlignment() const;
  uint32_t vertexBufferAlignment() const { return m_vertexBufferAlignment; }

  //--------------------------------------------------------------------------
  // Pipeline cache (device-lifetime resource)
  //--------------------------------------------------------------------------
  vk::PipelineCache pipelineCache() const { return m_pipelineCache.get(); }
  void savePipelineCache();

private:
  bool initDisplayDevice() const;
  bool initializeInstance();
  bool initializeSurface();
  bool pickPhysicalDevice(PhysicalDeviceValues &deviceValues);
  bool createLogicalDevice(const PhysicalDeviceValues &deviceValues);
  bool createSwapchain(const PhysicalDeviceValues &deviceValues);
  void createPipelineCache();
  void queryDeviceCapabilities(const PhysicalDeviceValues &deviceValues);

  vk::SurfaceFormatKHR chooseSurfaceFormat(const PhysicalDeviceValues &values) const;
  vk::PresentModeKHR choosePresentMode(const PhysicalDeviceValues &values) const;
  vk::Extent2D chooseSwapExtent(const PhysicalDeviceValues &values, uint32_t width, uint32_t height) const;

  std::unique_ptr<os::GraphicsOperations> m_graphicsOps;

  // Instance and debug
  vk::UniqueInstance m_instance;
  vk::UniqueDebugUtilsMessengerEXT m_debugMessenger;
  vk::UniqueSurfaceKHR m_surface;

  // Device
  vk::PhysicalDevice m_physicalDevice;
  vk::UniqueDevice m_device;
  vk::Queue m_graphicsQueue;
  vk::Queue m_presentQueue;
  uint32_t m_graphicsQueueIndex = 0;
  uint32_t m_presentQueueIndex = 0;

  // Device properties
  vk::PhysicalDeviceProperties m_properties;
  vk::PhysicalDeviceMemoryProperties m_memoryProperties;
  vk::PhysicalDeviceVulkan11Features m_features11;
  vk::PhysicalDeviceVulkan13Features m_features13;
  vk::PhysicalDeviceVulkan14Features m_features14;
  ExtendedDynamicState3Caps m_extDyn3Caps;
  bool m_supportsExtDyn = true;
  bool m_supportsExtDyn2 = false;
  bool m_supportsExtDyn3 = false;
  bool m_supportsVertexAttributeDivisor = false;
  uint32_t m_vertexBufferAlignment = static_cast<uint32_t>(sizeof(float));

  // Swapchain
  vk::UniqueSwapchainKHR m_swapchain;
  vk::Format m_swapchainFormat = vk::Format::eUndefined;
  vk::Extent2D m_swapchainExtent;
  vk::ImageUsageFlags m_swapchainUsage{};
  uint64_t m_swapchainGeneration = 0;
  SCP_vector<vk::Image> m_swapchainImages;
  SCP_vector<vk::UniqueImageView> m_swapchainImageViews;
  SCP_vector<vk::UniqueSemaphore> m_swapchainRenderFinishedSemaphores;

  // Pipeline cache
  vk::UniquePipelineCache m_pipelineCache;

  // Cached surface capabilities for recreation
  vk::SurfaceCapabilitiesKHR m_surfaceCapabilities;
  SCP_vector<vk::SurfaceFormatKHR> m_surfaceFormats;
  SCP_vector<vk::PresentModeKHR> m_presentModes;
};

} // namespace vulkan
} // namespace graphics
