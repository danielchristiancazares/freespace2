#include "VulkanDevice.h"
#include "VulkanDebug.h"
#include "VulkanModelValidation.h"

#include "cmdline/cmdline.h"
#include "globalincs/version.h"
#include "graphics/2d.h"
#include "mod_table/mod_table.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>

#if SDL_VERSION_ATLEAST(2, 0, 6)
#include <SDL_vulkan.h>
#endif

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace graphics {
namespace vulkan {

namespace {
#if SDL_SUPPORTS_VULKAN
const char *EngineName = "FreeSpaceOpen";

const gameversion::version MinVulkanVersion(1, 4, 0, 0);

uint32_t fnv1a32(const char *str) {
  uint32_t hash = 2166136261u;
  if (!str) {
    return hash;
  }

  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(str); *p; ++p) {
    hash ^= *p;
    hash *= 16777619u;
  }
  return hash;
}

enum class ValidationEmitKind { Skip, Normal, SuppressionNotice, Periodic };

ValidationEmitKind shouldEmitValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, uint32_t count) {
  const bool isError = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
  const uint32_t logFirst = isError ? 10u : 3u;
  const uint32_t logEvery = isError ? 50u : 200u;

  if (count <= logFirst) {
    return ValidationEmitKind::Normal;
  }
  if (count == logFirst + 1) {
    return ValidationEmitKind::SuppressionNotice;
  }
  if (count % logEvery == 0) {
    return ValidationEmitKind::Periodic;
  }
  return ValidationEmitKind::Skip;
}

std::string formatValidationTypes(VkDebugUtilsMessageTypeFlagsEXT types) {
  std::string out;
  auto add = [&out](const char *s) {
    if (!out.empty()) {
      out += "|";
    }
    out += s;
  };

  if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) {
    add("GENERAL");
  }
  if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
    add("VALIDATION");
  }
  if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
    add("PERFORMANCE");
  }
  if (out.empty()) {
    out = "UNKNOWN";
  }
  return out;
}

void logValidationObjects(const VkDebugUtilsMessengerCallbackDataEXT *data) {
  if (!data || data->objectCount == 0 || data->pObjects == nullptr) {
    return;
  }

  const uint32_t maxObjects = 8;
  const uint32_t count = std::min(data->objectCount, maxObjects);
  for (uint32_t i = 0; i < count; ++i) {
    const auto &obj = data->pObjects[i];
    const auto typeName = vk::to_string(static_cast<vk::ObjectType>(obj.objectType));
    const char *name = (obj.pObjectName != nullptr) ? obj.pObjectName : "<unnamed>";
    vkprintf("  object[%u]: type=%s handle=0x%llx name=%s\n", i, typeName.c_str(),
             static_cast<unsigned long long>(obj.objectHandle), name);
  }
  if (data->objectCount > maxObjects) {
    vkprintf("  object[%u+]: %u more suppressed\n", maxObjects, data->objectCount - maxObjects);
  }
}

void logValidationLabels(const VkDebugUtilsLabelEXT *labels, uint32_t count, const char *kind) {
  if (count == 0 || labels == nullptr || kind == nullptr) {
    return;
  }

  const uint32_t maxLabels = 8;
  const uint32_t n = std::min(count, maxLabels);
  for (uint32_t i = 0; i < n; ++i) {
    const char *name = (labels[i].pLabelName != nullptr) ? labels[i].pLabelName : "<unnamed>";
    vkprintf("  %sLabel[%u]: %s\n", kind, i, name);
  }
  if (count > maxLabels) {
    vkprintf("  %sLabel[%u+]: %u more suppressed\n", kind, maxLabels, count - maxLabels);
  }
}

VkBool32 VKAPI_PTR debugReportCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                       VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                                       const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
                                       void * /*pUserData*/) {
  // Keep validation visible during Vulkan work, but avoid log spam from repeated warnings/errors.
  static std::mutex s_mutex;
  static std::unordered_map<uint64_t, uint32_t> s_counts;

  const char *severity = "UNKNOWN";
  if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    severity = "ERROR";
  } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    severity = "WARN";
  } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
    severity = "INFO";
  } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
    severity = "VERBOSE";
  }

  const char *msg = (pCallbackData && pCallbackData->pMessage) ? pCallbackData->pMessage : "<null>";
  const char *msgIdName =
      (pCallbackData && pCallbackData->pMessageIdName) ? pCallbackData->pMessageIdName : "<no-id-name>";
  const int32_t msgIdNumber = pCallbackData ? pCallbackData->messageIdNumber : 0;

  uint32_t count = 0;
  ValidationEmitKind emit = ValidationEmitKind::Normal;
  {
    // Hash message id + type/severity so we can suppress repeated frame-to-frame spam.
    uint32_t nameHash = fnv1a32(msgIdName);
    nameHash ^= static_cast<uint32_t>(messageTypes) * 0x9e3779b1u;
    nameHash ^= static_cast<uint32_t>(messageSeverity) * 0x85ebca6bu;
    const uint64_t key =
        (static_cast<uint64_t>(static_cast<uint32_t>(msgIdNumber)) << 32) | static_cast<uint64_t>(nameHash);

    std::scoped_lock<std::mutex> lock(s_mutex);
    count = ++s_counts[key];
    emit = shouldEmitValidationMessage(messageSeverity, count);
  }

  if (emit == ValidationEmitKind::Skip) {
    return VK_FALSE;
  }

  const std::string typeStr = formatValidationTypes(messageTypes);
  if (emit == ValidationEmitKind::SuppressionNotice) {
    vkprintf("Validation[%s] [%s] id=%d name=%s (repeated; suppressing further duplicates): %s\n", severity,
             typeStr.c_str(), msgIdNumber, msgIdName, msg);
  } else if (emit == ValidationEmitKind::Periodic) {
    vkprintf("Validation[%s] [%s] id=%d name=%s (seen %u times): %s\n", severity, typeStr.c_str(), msgIdNumber,
             msgIdName, count, msg);
  } else {
    vkprintf("Validation[%s] [%s] id=%d name=%s: %s\n", severity, typeStr.c_str(), msgIdNumber, msgIdName, msg);
  }

  logValidationObjects(pCallbackData);
  if (pCallbackData) {
    logValidationLabels(pCallbackData->pQueueLabels, pCallbackData->queueLabelCount, "Queue");
    logValidationLabels(pCallbackData->pCmdBufLabels, pCallbackData->cmdBufLabelCount, "CmdBuf");
  }
  return VK_FALSE;
}
#endif

const SCP_vector<const char *> RequiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
};

const SCP_vector<const char *> OptionalDeviceExtensions = {
#ifdef VK_KHR_MAINTENANCE_6_EXTENSION_NAME
    VK_KHR_MAINTENANCE_6_EXTENSION_NAME,
#endif
#ifdef VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
#endif
#ifdef VK_EXT_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME
    VK_EXT_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME,
#endif
#ifdef VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME
    VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
#endif
};

bool checkDeviceExtensionSupport(PhysicalDeviceValues &values) {
  values.extensions = values.device.enumerateDeviceExtensionProperties();

  std::set<std::string> requiredExtensions(RequiredDeviceExtensions.cbegin(), RequiredDeviceExtensions.cend());
  for (const auto &extension : values.extensions) {
    requiredExtensions.erase(extension.extensionName);
  }

  return requiredExtensions.empty();
}

bool checkSwapChainSupport(PhysicalDeviceValues &values, const vk::UniqueSurfaceKHR &surface) {
  values.surfaceCapabilities = values.device.getSurfaceCapabilitiesKHR(surface.get());
  values.surfaceFormats = values.device.getSurfaceFormatsKHR(surface.get());
  values.presentModes = values.device.getSurfacePresentModesKHR(surface.get());

  return !values.surfaceFormats.empty() && !values.presentModes.empty();
}

bool isDeviceUnsuitable(PhysicalDeviceValues &values, const vk::UniqueSurfaceKHR &surface) {
  // We need a GPU. Reject CPU or "other" types.
  if (values.properties.deviceType != vk::PhysicalDeviceType::eDiscreteGpu &&
      values.properties.deviceType != vk::PhysicalDeviceType::eIntegratedGpu &&
      values.properties.deviceType != vk::PhysicalDeviceType::eVirtualGpu) {
    return true;
  }

  uint32_t i = 0;
  for (const auto &queue : values.queueProperties) {
    if (!values.graphicsQueueIndex.initialized && queue.queueFlags & vk::QueueFlagBits::eGraphics) {
      values.graphicsQueueIndex.initialized = true;
      values.graphicsQueueIndex.index = i;
    }
    if (!values.presentQueueIndex.initialized && values.device.getSurfaceSupportKHR(i, surface.get())) {
      values.presentQueueIndex.initialized = true;
      values.presentQueueIndex.index = i;
    }

    ++i;
  }

  if (!values.graphicsQueueIndex.initialized) {
    return true;
  }
  if (!values.presentQueueIndex.initialized) {
    return true;
  }

  if (!checkDeviceExtensionSupport(values)) {
    return true;
  }

  if (!checkSwapChainSupport(values, surface)) {
    return true;
  }

  if (values.properties.apiVersion < VK_API_VERSION_1_4) {
    return true;
  }

  // Push descriptors are required for the Vulkan model path.
  if (!ValidatePushDescriptorSupport(values.features14)) {
    return true;
  }

  // Descriptor indexing features are required for the Vulkan model path (bindless textures).
  if (!ValidateModelDescriptorIndexingSupport(values.features12)) {
    return true;
  }

  // Dynamic rendering is required for the engine's renderPass-less pipelines.
  if (values.features13.dynamicRendering != VK_TRUE) {
    return true;
  }

  return false;
}

} // namespace

uint32_t deviceTypeScore(vk::PhysicalDeviceType type) {
  switch (type) {
  case vk::PhysicalDeviceType::eDiscreteGpu:
    return 3;
  case vk::PhysicalDeviceType::eIntegratedGpu:
    return 2;
  case vk::PhysicalDeviceType::eVirtualGpu:
    return 1;
  case vk::PhysicalDeviceType::eCpu:
  case vk::PhysicalDeviceType::eOther:
  default:
    return 0;
  }
}

uint32_t scoreDevice(const PhysicalDeviceValues &device) {
  uint32_t score = 0;

  // Device type is the dominant factor (discrete > integrated > virtual > other)
  score += deviceTypeScore(device.properties.deviceType) * 1000000;

  // Vulkan version as tiebreaker between same-type devices
  // Use major.minor only; patch version is irrelevant for capability
  uint32_t major = VK_VERSION_MAJOR(device.properties.apiVersion);
  uint32_t minor = VK_VERSION_MINOR(device.properties.apiVersion);
  score += major * 100 + minor;

  return score;
}

namespace {

bool compareDevices(const PhysicalDeviceValues &left, const PhysicalDeviceValues &right) {
  return scoreDevice(left) < scoreDevice(right);
}

void printPhysicalDevice(const PhysicalDeviceValues &values) {}

} // namespace

VulkanDevice::VulkanDevice(std::unique_ptr<os::GraphicsOperations> graphicsOps)
    : m_graphicsOps(std::move(graphicsOps)) {}

VulkanDevice::~VulkanDevice() { shutdown(); }

bool VulkanDevice::initialize() {
  if (!initDisplayDevice()) {
    return false;
  }

  if (!initializeInstance()) {
    return false;
  }

  if (!initializeSurface()) {
    return false;
  }

  PhysicalDeviceValues deviceValues;
  if (!pickPhysicalDevice(deviceValues)) {
    return false;
  }

  if (!createLogicalDevice(deviceValues)) {
    return false;
  }

  if (!createSwapchain(deviceValues)) {
    return false;
  }

  // Store queue indices
  m_graphicsQueueIndex = deviceValues.graphicsQueueIndex.index;
  m_presentQueueIndex = deviceValues.presentQueueIndex.index;

  // Query and store device capabilities
  queryDeviceCapabilities(deviceValues);

  // Create pipeline cache
  createPipelineCache();

  // Cache surface capabilities for swapchain recreation
  m_surfaceCapabilities = deviceValues.surfaceCapabilities;
  m_surfaceFormats =
      SCP_vector<vk::SurfaceFormatKHR>(deviceValues.surfaceFormats.begin(), deviceValues.surfaceFormats.end());
  m_presentModes = SCP_vector<vk::PresentModeKHR>(deviceValues.presentModes.begin(), deviceValues.presentModes.end());

  return true;
}

void VulkanDevice::shutdown() {
  if (!m_device) {
    return; // Already shut down or never initialized
  }

  m_device->waitIdle();

  // Save pipeline cache while device is still valid
  savePipelineCache();

  // Clear non-owned handles (swapchain images are owned by the swapchain, not us)
  m_swapchainImages.clear();

  // RAII handles are cleaned up by destructors in reverse declaration order
}

bool VulkanDevice::initDisplayDevice() const {
  os::ViewPortProperties attrs;
  attrs.enable_opengl = false;
  attrs.enable_vulkan = true;

  attrs.display = os_config_read_uint("Video", "Display", 0);
  attrs.width = static_cast<uint32_t>(gr_screen.max_w);
  attrs.height = static_cast<uint32_t>(gr_screen.max_h);

  attrs.title = Osreg_title;
  if (!Window_title.empty()) {
    attrs.title = Window_title;
  }

  if (Using_in_game_options) {
    switch (Gr_configured_window_state) {
    case os::ViewportState::Windowed:
      // That's the default
      break;
    case os::ViewportState::Borderless:
      attrs.flags.set(os::ViewPortFlags::Borderless);
      break;
    case os::ViewportState::Fullscreen:
      attrs.flags.set(os::ViewPortFlags::Fullscreen);
      break;
    }
  } else {
    if (!Cmdline_window && !Cmdline_fullscreen_window) {
      attrs.flags.set(os::ViewPortFlags::Fullscreen);
    } else if (Cmdline_fullscreen_window) {
      attrs.flags.set(os::ViewPortFlags::Borderless);
    }
  }

  if (Cmdline_capture_mouse)
    attrs.flags.set(os::ViewPortFlags::Capture_Mouse);

  auto viewPort = m_graphicsOps->createViewport(attrs);
  if (!viewPort) {
    return false;
  }

  const auto port = os::addViewport(std::move(viewPort));
  os::setMainViewPort(port);

  return true;
}

bool VulkanDevice::initializeInstance() {
#if SDL_SUPPORTS_VULKAN
  const auto vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());

  if (!vkGetInstanceProcAddr) {
    return false;
  }

  VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

  const auto window = os::getSDLMainWindow();

  unsigned int count;
  if (!SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr)) {
    return false;
  }

  std::vector<const char *> extensions;
  extensions.resize(count);

  if (!SDL_Vulkan_GetInstanceExtensions(window, &count, extensions.data())) {
    return false;
  }

  const auto instanceVersion = vk::enumerateInstanceVersion();
  gameversion::version vulkanVersion(VK_VERSION_MAJOR(instanceVersion), VK_VERSION_MINOR(instanceVersion),
                                     VK_VERSION_PATCH(instanceVersion), 0);

  if (vulkanVersion < MinVulkanVersion) {
    return false;
  }

  const auto supportedExtensions = vk::enumerateInstanceExtensionProperties();
  for (const auto &ext : supportedExtensions) {
    if (FSO_DEBUG || Cmdline_graphics_debug_output) {
      if (!stricmp(ext.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
      }
    }
#if defined(__APPLE__) && defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
    // Portability enumeration is required for MoltenVK/portability subset drivers on Apple, but
    // RenderDoc's Vulkan layer may reject it on other platforms (breaking vkCreateInstance).
    if (!stricmp(ext.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
      extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
#endif
  }

  std::vector<const char *> layers;
  const auto supportedLayers = vk::enumerateInstanceLayerProperties();
  for (const auto &layer : supportedLayers) {
    if (FSO_DEBUG || Cmdline_graphics_debug_output) {
      if (!stricmp(layer.layerName, "VK_LAYER_KHRONOS_validation")) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
      }
    }
  }

  vk::ApplicationInfo appInfo(Window_title.c_str(), 1, EngineName, 1, VK_API_VERSION_1_4);

  // Now we can make the Vulkan instance
  vk::InstanceCreateInfo createInfo(vk::Flags<vk::InstanceCreateFlagBits>(), &appInfo);
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.data();
  createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
  createInfo.ppEnabledLayerNames = layers.data();
#if defined(__APPLE__) && defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
  if (std::find_if(extensions.begin(), extensions.end(), [](const char *ext) {
        return !stricmp(ext, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
      }) != extensions.end()) {
    createInfo.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
  }
#endif

  vk::DebugUtilsMessengerCreateInfoEXT createInstanceDebugInfo(
      {}, vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
      vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
          vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
      debugReportCallback);

  vk::StructureChain<vk::InstanceCreateInfo, vk::DebugUtilsMessengerCreateInfoEXT> createInstanceChain(
      createInfo, createInstanceDebugInfo);

  if (!(FSO_DEBUG || Cmdline_graphics_debug_output)) {
    createInstanceChain.unlink<vk::DebugUtilsMessengerCreateInfoEXT>();
  }

  vk::UniqueInstance instance = vk::createInstanceUnique(createInstanceChain.get<vk::InstanceCreateInfo>(), nullptr);
  if (!instance) {
    return false;
  }

  VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.get());

  if (FSO_DEBUG || Cmdline_graphics_debug_output) {
    m_debugMessenger = instance->createDebugUtilsMessengerEXTUnique(createInstanceDebugInfo);
  }

  m_instance = std::move(instance);
  return true;
#else
  return false;
#endif
}

bool VulkanDevice::initializeSurface() {
#if SDL_SUPPORTS_VULKAN
  const auto window = os::getSDLMainWindow();

  VkSurfaceKHR surface;
  if (!SDL_Vulkan_CreateSurface(window, static_cast<VkInstance>(*m_instance), &surface)) {
    return false;
  }

#if VK_HEADER_VERSION >= 301
  const vk::detail::ObjectDestroy<vk::Instance, VULKAN_HPP_DEFAULT_DISPATCHER_TYPE> deleter(
      *m_instance,
#else
  const vk::ObjectDestroy<vk::Instance, VULKAN_HPP_DEFAULT_DISPATCHER_TYPE> deleter(
      *m_instance,
#endif
      nullptr, VULKAN_HPP_DEFAULT_DISPATCHER);
  m_surface = vk::UniqueSurfaceKHR(vk::SurfaceKHR(surface), deleter);
  return true;
#else
  return false;
#endif
}

bool VulkanDevice::pickPhysicalDevice(PhysicalDeviceValues &deviceValues) {
  const auto devices = m_instance->enumeratePhysicalDevices();
  if (devices.empty()) {
    return false;
  }

  SCP_vector<PhysicalDeviceValues> values;
  std::transform(devices.cbegin(), devices.cend(), std::back_inserter(values), [](const vk::PhysicalDevice &dev) {
    PhysicalDeviceValues vals;
    vals.device = dev;

    vk::PhysicalDeviceProperties2 props;
    dev.getProperties2(&props);
    vals.properties = props.properties;

    vk::PhysicalDeviceFeatures2 feats;
    vals.features11 = vk::PhysicalDeviceVulkan11Features{};
    vals.features12 = vk::PhysicalDeviceVulkan12Features{};
    vals.features13 = vk::PhysicalDeviceVulkan13Features{};
    vals.features14 = vk::PhysicalDeviceVulkan14Features{};
    vals.extDynamicState = vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT{};
    vals.extDynamicState2 = vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT{};
    vals.extDynamicState3 = vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT{};
    vals.pushDescriptorProps = vk::PhysicalDevicePushDescriptorPropertiesKHR{};
    feats.pNext = &vals.features11;
    vals.features11.pNext = &vals.features12;
    vals.features12.pNext = &vals.features13;
    vals.features13.pNext = &vals.features14;
    vals.features14.pNext = &vals.extDynamicState;
    vals.extDynamicState.pNext = &vals.extDynamicState2;
    vals.extDynamicState2.pNext = &vals.extDynamicState3;
    vals.extDynamicState3.pNext = &vals.pushDescriptorProps;
    dev.getFeatures2(&feats);
    vals.features = feats.features;

    vals.queueProperties = dev.getQueueFamilyProperties();
    return vals;
  });

  // Remove devices that do not have the features we need
  values.erase(std::remove_if(values.begin(), values.end(),
                              [this](PhysicalDeviceValues &value) { return isDeviceUnsuitable(value, m_surface); }),
               values.end());
  if (values.empty()) {
    return false;
  }

  // Sort the suitability of the devices in increasing order
  std::sort(values.begin(), values.end(), compareDevices);

  deviceValues = values.back();

  return true;
}

bool VulkanDevice::createLogicalDevice(const PhysicalDeviceValues &deviceValues) {
  m_physicalDevice = deviceValues.device;

  float queuePriority = 1.0f;

  std::vector<vk::DeviceQueueCreateInfo> queueInfos;
  const std::set<uint32_t> familyIndices{deviceValues.graphicsQueueIndex.index, deviceValues.presentQueueIndex.index};

  queueInfos.reserve(familyIndices.size());
  for (auto index : familyIndices) {
    queueInfos.emplace_back(vk::DeviceQueueCreateFlags(), index, 1, &queuePriority);
  }

  // Enable required/optional extensions
  std::vector<const char *> enabledExtensions(RequiredDeviceExtensions.begin(), RequiredDeviceExtensions.end());
  for (const auto &opt : OptionalDeviceExtensions) {
    auto it = std::find_if(
        deviceValues.extensions.begin(), deviceValues.extensions.end(),
        [&opt](const vk::ExtensionProperties &prop) { return std::strcmp(prop.extensionName.data(), opt) == 0; });
    if (it != deviceValues.extensions.end()) {
      enabledExtensions.push_back(opt);
    }
  }

  // Chain features for 1.2/1.3/1.4 and extension features
  // Create fresh structures to avoid copying stale pNext pointers from the query phase
  vk::PhysicalDeviceFeatures2 enabledFeatures;
  enabledFeatures.features = deviceValues.features;
  vk::PhysicalDeviceVulkan11Features enabled11 = deviceValues.features11;
  vk::PhysicalDeviceVulkan12Features enabled12 = deviceValues.features12;
  vk::PhysicalDeviceVulkan13Features enabled13 = deviceValues.features13;
  vk::PhysicalDeviceVulkan14Features enabled14 = deviceValues.features14;
  enabled11.samplerYcbcrConversion = deviceValues.features11.samplerYcbcrConversion ? VK_TRUE : VK_FALSE;
  // Explicitly enable the features we depend on (after capability checks).
  enabled13.dynamicRendering = deviceValues.features13.dynamicRendering ? VK_TRUE : VK_FALSE;
  enabled14.vertexAttributeInstanceRateDivisor = deviceValues.features14.vertexAttributeInstanceRateDivisor;
  vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT enabledExtDyn = deviceValues.extDynamicState;
  vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT enabledExtDyn2 = deviceValues.extDynamicState2;
  vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT enabledExtDyn3 = deviceValues.extDynamicState3;
  // Explicitly reset pNext to ensure clean chain (copied structures may have stale pointers)
  enabled11.pNext = &enabled12;
  enabled12.pNext = &enabled13;
  enabled13.pNext = &enabled14;
  enabled14.pNext = &enabledExtDyn;
  enabledExtDyn.pNext = &enabledExtDyn2;
  enabledExtDyn2.pNext = &enabledExtDyn3;
  enabledExtDyn3.pNext = nullptr;
  enabledFeatures.pNext = &enabled11;

  vk::DeviceCreateInfo deviceCreate;
  deviceCreate.pQueueCreateInfos = queueInfos.data();
  deviceCreate.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
  deviceCreate.pEnabledFeatures = nullptr;
  deviceCreate.pNext = &enabledFeatures;

  deviceCreate.ppEnabledExtensionNames = enabledExtensions.data();
  deviceCreate.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());

  m_device = deviceValues.device.createDeviceUnique(deviceCreate);

  // Initialize default dispatcher for device-level functions
  VULKAN_HPP_DEFAULT_DISPATCHER.init(m_device.get());

  // Create queues
  m_graphicsQueue = m_device->getQueue(deviceValues.graphicsQueueIndex.index, 0);
  m_presentQueue = m_device->getQueue(deviceValues.presentQueueIndex.index, 0);

  // Store device properties
  m_properties = deviceValues.properties;
  m_memoryProperties = m_physicalDevice.getMemoryProperties();

  // Keep sanitized copies of enabled Vulkan features for downstream validation.
  m_features11 = enabled11;
  m_features11.pNext = nullptr;

  m_features13 = enabled13;
  m_features13.pNext = nullptr;

  m_features14 = enabled14;
  m_features14.pNext = nullptr;

  return true;
}

bool VulkanDevice::createSwapchain(const PhysicalDeviceValues &deviceValues) {
  // Choose one more than the minimum to avoid driver synchronization if it is not done with a thread yet
  uint32_t imageCount = deviceValues.surfaceCapabilities.minImageCount + 1;
  if (deviceValues.surfaceCapabilities.maxImageCount > 0 &&
      imageCount > deviceValues.surfaceCapabilities.maxImageCount) {
    imageCount = deviceValues.surfaceCapabilities.maxImageCount;
  }

  const auto surfaceFormat = chooseSurfaceFormat(deviceValues);

  vk::SwapchainCreateInfoKHR createInfo;
  createInfo.surface = m_surface.get();
  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = chooseSwapExtent(deviceValues, gr_screen.max_w, gr_screen.max_h);
  if (createInfo.imageExtent.width == 0 || createInfo.imageExtent.height == 0) {
    vkprintf("VulkanDevice: swapchain extent is 0x0; window likely minimized; cannot create swapchain.\n");
    return false;
  }
  createInfo.imageArrayLayers = 1;
  {
    // We need transfer-src so the Vulkan backend can snapshot pre-deferred scene color (OpenGL parity).
    // Only request usages explicitly supported by the surface.
    const auto supported = deviceValues.surfaceCapabilities.supportedUsageFlags;
    const auto requested = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc;
    createInfo.imageUsage = requested & supported;
    Assertion((createInfo.imageUsage & vk::ImageUsageFlagBits::eColorAttachment) != vk::ImageUsageFlags{},
              "Surface does not support swapchain images as color attachments (supportedUsageFlags=0x%x)",
              static_cast<uint32_t>(supported));
    if ((createInfo.imageUsage & vk::ImageUsageFlagBits::eTransferSrc) == vk::ImageUsageFlags{}) {
      vkprintf("VulkanDevice: swapchain does not support TRANSFER_SRC usage; "
               "pre-deferred scene capture will be disabled.\n");
    }
  }

  const uint32_t queueFamilyIndices[] = {deviceValues.graphicsQueueIndex.index, deviceValues.presentQueueIndex.index};
  if (deviceValues.graphicsQueueIndex.index != deviceValues.presentQueueIndex.index) {
    createInfo.imageSharingMode = vk::SharingMode::eConcurrent;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = queueFamilyIndices;
  } else {
    createInfo.imageSharingMode = vk::SharingMode::eExclusive;
    createInfo.queueFamilyIndexCount = 0;     // Optional
    createInfo.pQueueFamilyIndices = nullptr; // Optional
  }

  createInfo.preTransform = deviceValues.surfaceCapabilities.currentTransform;
  createInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
  createInfo.presentMode = choosePresentMode(deviceValues);
  createInfo.clipped = true;
  createInfo.oldSwapchain = nullptr;

  m_swapchain = m_device->createSwapchainKHRUnique(createInfo);

  std::vector<vk::Image> swapchainImages = m_device->getSwapchainImagesKHR(m_swapchain.get());
  m_swapchainImages = SCP_vector<vk::Image>(swapchainImages.begin(), swapchainImages.end());
  m_swapchainFormat = surfaceFormat.format;
  m_swapchainExtent = createInfo.imageExtent;
  m_swapchainUsage = createInfo.imageUsage;
  ++m_swapchainGeneration;

  m_swapchainImageViews.reserve(m_swapchainImages.size());
  for (const auto &image : m_swapchainImages) {
    vk::ImageViewCreateInfo viewCreateInfo;
    viewCreateInfo.image = image;
    viewCreateInfo.viewType = vk::ImageViewType::e2D;
    viewCreateInfo.format = m_swapchainFormat;

    viewCreateInfo.components.r = vk::ComponentSwizzle::eIdentity;
    viewCreateInfo.components.g = vk::ComponentSwizzle::eIdentity;
    viewCreateInfo.components.b = vk::ComponentSwizzle::eIdentity;
    viewCreateInfo.components.a = vk::ComponentSwizzle::eIdentity;

    viewCreateInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewCreateInfo.subresourceRange.baseMipLevel = 0;
    viewCreateInfo.subresourceRange.levelCount = 1;
    viewCreateInfo.subresourceRange.baseArrayLayer = 0;
    viewCreateInfo.subresourceRange.layerCount = 1;

    m_swapchainImageViews.push_back(m_device->createImageViewUnique(viewCreateInfo));
  }

  // Render-finished semaphores are indexed by swapchain image to avoid reuse hazards with presentation.
  m_swapchainRenderFinishedSemaphores.clear();
  m_swapchainRenderFinishedSemaphores.reserve(m_swapchainImages.size());
  vk::SemaphoreCreateInfo semInfo{};
  for (size_t i = 0; i < m_swapchainImages.size(); ++i) {
    m_swapchainRenderFinishedSemaphores.push_back(m_device->createSemaphoreUnique(semInfo));
  }

  return true;
}

void VulkanDevice::queryDeviceCapabilities(const PhysicalDeviceValues &deviceValues) {
  m_vertexBufferAlignment = static_cast<uint32_t>(sizeof(float));

  // Query extended dynamic state 3 capabilities per-feature
  if (std::find_if(deviceValues.extensions.begin(), deviceValues.extensions.end(),
                   [](const vk::ExtensionProperties &prop) {
                     return std::strcmp(prop.extensionName.data(), VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) == 0;
                   }) != deviceValues.extensions.end()) {
    m_supportsExtDyn3 = true;

    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT extDyn3Feats{};
    vk::PhysicalDeviceFeatures2 feats2;
    feats2.pNext = &extDyn3Feats;
    m_physicalDevice.getFeatures2(&feats2);

    m_extDyn3Caps.colorBlendEnable = extDyn3Feats.extendedDynamicState3ColorBlendEnable;
    m_extDyn3Caps.colorWriteMask = extDyn3Feats.extendedDynamicState3ColorWriteMask;
    m_extDyn3Caps.polygonMode = extDyn3Feats.extendedDynamicState3PolygonMode;
    m_extDyn3Caps.rasterizationSamples = extDyn3Feats.extendedDynamicState3RasterizationSamples;
  }

  // Prefer core vertex attribute divisor support (Vulkan 1.4 promotion).
  m_supportsVertexAttributeDivisor = m_features14.vertexAttributeInstanceRateDivisor;

  // If core support is absent, fall back to the extension (for forward compatibility/testing).
  if (!m_supportsVertexAttributeDivisor &&
      std::find_if(deviceValues.extensions.begin(), deviceValues.extensions.end(),
                   [](const vk::ExtensionProperties &prop) {
                     return std::strcmp(prop.extensionName.data(), VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0;
                   }) != deviceValues.extensions.end()) {
    vk::PhysicalDeviceVertexAttributeDivisorFeaturesEXT divisorFeats{};
    vk::PhysicalDeviceFeatures2 feats2;
    feats2.pNext = &divisorFeats;
    m_physicalDevice.getFeatures2(&feats2);
    m_supportsVertexAttributeDivisor = divisorFeats.vertexAttributeInstanceRateDivisor;
  }
}

void VulkanDevice::createPipelineCache() {
  // Pipeline cache header structure for UUID/driver validation
  struct CacheHeader {
    uint32_t headerLength;
    uint32_t headerVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint8_t pipelineCacheUUID[VK_UUID_SIZE];
  };

  std::vector<char> cacheData;
  const std::filesystem::path cachePath("vulkan_pipeline.cache");
  bool validCache = false;

  if (std::filesystem::exists(cachePath)) {
    std::ifstream cacheFile(cachePath, std::ios::binary | std::ios::ate);
    if (cacheFile) {
      auto size = cacheFile.tellg();
      cacheFile.seekg(0);
      cacheData.resize(static_cast<size_t>(size));
      cacheFile.read(cacheData.data(), size);
      cacheFile.close();

      // Validate cache header if present
      if (cacheData.size() >= sizeof(CacheHeader)) {
        const auto *header = reinterpret_cast<const CacheHeader *>(cacheData.data());
        if (header->headerLength == sizeof(CacheHeader) && header->vendorID == m_properties.vendorID &&
            header->deviceID == m_properties.deviceID &&
            std::memcmp(header->pipelineCacheUUID, m_properties.pipelineCacheUUID, VK_UUID_SIZE) == 0) {
          validCache = true;
          // Skip header when creating cache
          vk::PipelineCacheCreateInfo cacheInfo;
          cacheInfo.initialDataSize = cacheData.size() - sizeof(CacheHeader);
          cacheInfo.pInitialData = cacheData.data() + sizeof(CacheHeader);
          m_pipelineCache = m_device->createPipelineCacheUnique(cacheInfo);
        }
      }
    }
  }

  if (!validCache) {
    vk::PipelineCacheCreateInfo cacheInfo;
    m_pipelineCache = m_device->createPipelineCacheUnique(cacheInfo);
  }
}

void VulkanDevice::savePipelineCache() {
  if (!m_pipelineCache) {
    return;
  }

  struct CacheHeader {
    uint32_t headerLength;
    uint32_t headerVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint8_t pipelineCacheUUID[VK_UUID_SIZE];
  };

  auto cacheData = m_device->getPipelineCacheData(m_pipelineCache.get());
  if (!cacheData.empty()) {
    const std::filesystem::path cachePath("vulkan_pipeline.cache");
    std::ofstream cacheFile(cachePath, std::ios::binary);
    if (cacheFile) {
      CacheHeader header{};
      header.headerLength = sizeof(CacheHeader);
      header.headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
      header.vendorID = m_properties.vendorID;
      header.deviceID = m_properties.deviceID;
      std::memcpy(header.pipelineCacheUUID, m_properties.pipelineCacheUUID, VK_UUID_SIZE);
      cacheFile.write(reinterpret_cast<const char *>(&header), sizeof(header));
      cacheFile.write(reinterpret_cast<const char *>(cacheData.data()), cacheData.size());
    }
  }
}

vk::SurfaceFormatKHR VulkanDevice::chooseSurfaceFormat(const PhysicalDeviceValues &values) const {
  for (const auto &availableFormat : values.surfaceFormats) {
    // Simple check is enough for now
    if (availableFormat.format == vk::Format::eB8G8R8A8Srgb &&
        availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
      return availableFormat;
    }
  }

  return values.surfaceFormats.front();
}

vk::PresentModeKHR VulkanDevice::choosePresentMode(const PhysicalDeviceValues &values) const {
  // Depending on if we want Vsync or not, choose the best mode
  for (const auto &availablePresentMode : values.presentModes) {
    if (Gr_enable_vsync) {
      if (availablePresentMode == vk::PresentModeKHR::eMailbox) {
        return availablePresentMode;
      }
    } else {
      if (availablePresentMode == vk::PresentModeKHR::eImmediate) {
        return availablePresentMode;
      }
    }
  }

  // Guaranteed to be supported
  return vk::PresentModeKHR::eFifo;
}

vk::Extent2D VulkanDevice::chooseSwapExtent(const PhysicalDeviceValues &values, uint32_t width, uint32_t height) const {
  if (values.surfaceCapabilities.currentExtent.width != UINT32_MAX) {
    return values.surfaceCapabilities.currentExtent;
  } else {
    VkExtent2D actualExtent = {width, height};

    actualExtent.width = std::max(values.surfaceCapabilities.minImageExtent.width,
                                  std::min(values.surfaceCapabilities.maxImageExtent.width, actualExtent.width));
    actualExtent.height = std::max(values.surfaceCapabilities.minImageExtent.height,
                                   std::min(values.surfaceCapabilities.maxImageExtent.height, actualExtent.height));

    return actualExtent;
  }
}

VulkanDevice::AcquireResult VulkanDevice::acquireNextImage(vk::Semaphore imageAvailable) {
  AcquireResult result;
  uint32_t imageIndex = std::numeric_limits<uint32_t>::max();

  vk::Result res = vk::Result::eSuccess;
  try {
    res = m_device->acquireNextImageKHR(m_swapchain.get(), std::numeric_limits<uint64_t>::max(), imageAvailable,
                                        nullptr, &imageIndex);
  } catch (const vk::SystemError &err) {
    res = static_cast<vk::Result>(err.code().value());
  }

  if (res == vk::Result::eErrorOutOfDateKHR || res == vk::Result::eSuboptimalKHR) {
    result.needsRecreate = true;
    result.success = (res == vk::Result::eSuboptimalKHR);
    result.imageIndex = imageIndex;
    return result;
  }

  if (res != vk::Result::eSuccess) {
    result.success = false;
    return result;
  }

  result.imageIndex = imageIndex;
  result.success = true;
  return result;
}

VulkanDevice::PresentResult VulkanDevice::present(vk::Semaphore renderFinished, uint32_t imageIndex) {
  PresentResult result;

  vk::PresentInfoKHR presentInfo;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = &renderFinished;
  presentInfo.swapchainCount = 1;
  auto swapchain = m_swapchain.get();
  presentInfo.pSwapchains = &swapchain;
  presentInfo.pImageIndices = &imageIndex;

  vk::Result presentResult = vk::Result::eSuccess;
  try {
    presentResult = m_presentQueue.presentKHR(presentInfo);
  } catch (const vk::SystemError &err) {
    presentResult = static_cast<vk::Result>(err.code().value());
  }

  if (presentResult == vk::Result::eErrorOutOfDateKHR || presentResult == vk::Result::eSuboptimalKHR) {
    result.needsRecreate = true;
    result.success = (presentResult == vk::Result::eSuboptimalKHR);
    return result;
  }

  if (presentResult != vk::Result::eSuccess) {
    result.success = false;
    return result;
  }

  result.success = true;
  return result;
}

bool VulkanDevice::recreateSwapchain(uint32_t width, uint32_t height) {
  // Re-query surface capabilities first; if the surface is minimized (0x0), a swapchain cannot be created.
  const auto newSurfaceCaps = m_physicalDevice.getSurfaceCapabilitiesKHR(m_surface.get());
  if (newSurfaceCaps.currentExtent.width == 0 || newSurfaceCaps.currentExtent.height == 0) {
    return false;
  }

  m_device->waitIdle();

  m_swapchainImageViews.clear();
  m_swapchainRenderFinishedSemaphores.clear();
  auto oldSwapchain = std::move(m_swapchain);

  // Re-query surface capabilities
  m_surfaceCapabilities = newSurfaceCaps;
  m_surfaceFormats.clear();
  auto formats = m_physicalDevice.getSurfaceFormatsKHR(m_surface.get());
  m_surfaceFormats = SCP_vector<vk::SurfaceFormatKHR>(formats.begin(), formats.end());
  auto modes = m_physicalDevice.getSurfacePresentModesKHR(m_surface.get());
  m_presentModes = SCP_vector<vk::PresentModeKHR>(modes.begin(), modes.end());

  // Build PhysicalDeviceValues from cached data
  PhysicalDeviceValues tempValues;
  tempValues.surfaceCapabilities = m_surfaceCapabilities;
  tempValues.surfaceFormats = std::vector<vk::SurfaceFormatKHR>(m_surfaceFormats.begin(), m_surfaceFormats.end());
  tempValues.presentModes = std::vector<vk::PresentModeKHR>(m_presentModes.begin(), m_presentModes.end());
  tempValues.graphicsQueueIndex.index = m_graphicsQueueIndex;
  tempValues.graphicsQueueIndex.initialized = true;
  tempValues.presentQueueIndex.index = m_presentQueueIndex;
  tempValues.presentQueueIndex.initialized = true;

  uint32_t imageCount = m_surfaceCapabilities.minImageCount + 1;
  if (m_surfaceCapabilities.maxImageCount > 0 && imageCount > m_surfaceCapabilities.maxImageCount) {
    imageCount = m_surfaceCapabilities.maxImageCount;
  }

  vk::SurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(tempValues);

  vk::SwapchainCreateInfoKHR createInfo;
  createInfo.surface = m_surface.get();
  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = chooseSwapExtent(tempValues, width, height);
  if (createInfo.imageExtent.width == 0 || createInfo.imageExtent.height == 0) {
    m_swapchain = std::move(oldSwapchain);
    return false;
  }
  createInfo.imageArrayLayers = 1;
  {
    const auto supported = m_surfaceCapabilities.supportedUsageFlags;
    const auto requested = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc;
    createInfo.imageUsage = requested & supported;
    Assertion((createInfo.imageUsage & vk::ImageUsageFlagBits::eColorAttachment) != vk::ImageUsageFlags{},
              "Surface does not support swapchain images as color attachments (supportedUsageFlags=0x%x)",
              static_cast<uint32_t>(supported));
    if ((createInfo.imageUsage & vk::ImageUsageFlagBits::eTransferSrc) == vk::ImageUsageFlags{}) {
      vkprintf("VulkanDevice: swapchain does not support TRANSFER_SRC usage; "
               "pre-deferred scene capture will be disabled.\n");
    }
  }

  const uint32_t queueFamilyIndices[] = {m_graphicsQueueIndex, m_presentQueueIndex};
  if (m_graphicsQueueIndex != m_presentQueueIndex) {
    createInfo.imageSharingMode = vk::SharingMode::eConcurrent;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = queueFamilyIndices;
  } else {
    createInfo.imageSharingMode = vk::SharingMode::eExclusive;
    createInfo.queueFamilyIndexCount = 0;
    createInfo.pQueueFamilyIndices = nullptr;
  }

  createInfo.preTransform = m_surfaceCapabilities.currentTransform;
  createInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
  createInfo.presentMode = choosePresentMode(tempValues);
  createInfo.clipped = true;
  createInfo.oldSwapchain = oldSwapchain.get();

  try {
    m_swapchain = m_device->createSwapchainKHRUnique(createInfo);
  } catch (const vk::SystemError &) {
    m_swapchain = std::move(oldSwapchain);
    return false;
  }

  oldSwapchain.reset();

  std::vector<vk::Image> swapchainImages = m_device->getSwapchainImagesKHR(m_swapchain.get());
  m_swapchainImages = SCP_vector<vk::Image>(swapchainImages.begin(), swapchainImages.end());
  m_swapchainFormat = surfaceFormat.format;
  m_swapchainExtent = createInfo.imageExtent;
  m_swapchainUsage = createInfo.imageUsage;
  ++m_swapchainGeneration;

  m_swapchainImageViews.reserve(m_swapchainImages.size());
  for (const auto &image : m_swapchainImages) {
    vk::ImageViewCreateInfo viewCreateInfo;
    viewCreateInfo.image = image;
    viewCreateInfo.viewType = vk::ImageViewType::e2D;
    viewCreateInfo.format = m_swapchainFormat;

    viewCreateInfo.components.r = vk::ComponentSwizzle::eIdentity;
    viewCreateInfo.components.g = vk::ComponentSwizzle::eIdentity;
    viewCreateInfo.components.b = vk::ComponentSwizzle::eIdentity;
    viewCreateInfo.components.a = vk::ComponentSwizzle::eIdentity;

    viewCreateInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewCreateInfo.subresourceRange.baseMipLevel = 0;
    viewCreateInfo.subresourceRange.levelCount = 1;
    viewCreateInfo.subresourceRange.baseArrayLayer = 0;
    viewCreateInfo.subresourceRange.layerCount = 1;

    m_swapchainImageViews.push_back(m_device->createImageViewUnique(viewCreateInfo));
  }

  // Render-finished semaphores are indexed by swapchain image to avoid reuse hazards with presentation.
  m_swapchainRenderFinishedSemaphores.reserve(m_swapchainImages.size());
  vk::SemaphoreCreateInfo semInfo{};
  for (size_t i = 0; i < m_swapchainImages.size(); ++i) {
    m_swapchainRenderFinishedSemaphores.push_back(m_device->createSemaphoreUnique(semInfo));
  }

  return true;
}

vk::Image VulkanDevice::swapchainImage(uint32_t index) const {
  if (index >= m_swapchainImages.size()) {
    return vk::Image{};
  }
  return m_swapchainImages[index];
}

vk::ImageView VulkanDevice::swapchainImageView(uint32_t index) const {
  if (index >= m_swapchainImageViews.size()) {
    return vk::ImageView{};
  }
  return m_swapchainImageViews[index].get();
}

uint32_t VulkanDevice::swapchainImageCount() const { return static_cast<uint32_t>(m_swapchainImages.size()); }

vk::Semaphore VulkanDevice::swapchainRenderFinishedSemaphore(uint32_t imageIndex) const {
  if (imageIndex >= m_swapchainRenderFinishedSemaphores.size()) {
    return vk::Semaphore{};
  }
  return m_swapchainRenderFinishedSemaphores[imageIndex].get();
}

uint32_t VulkanDevice::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const {
  for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i) {
    if ((typeFilter & (1u << i)) && (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  throw std::runtime_error("Failed to find suitable memory type.");
}

size_t VulkanDevice::minUniformBufferOffsetAlignment() const {
  return m_properties.limits.minUniformBufferOffsetAlignment;
}

} // namespace vulkan
} // namespace graphics
