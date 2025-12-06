
#include "VulkanRenderer.h"
#include "gr_vulkan.h"
#include "VulkanConstants.h"
#include "graphics/util/uniform_structs.h"

#include "globalincs/version.h"

#include "backends/imgui_impl_sdl.h"
#include "backends/imgui_impl_vulkan.h"
#include "def_files/def_files.h"
#include "graphics/2d.h"
#include "libs/renderdoc/renderdoc.h"
#include "mod_table/mod_table.h"
#include "VulkanModelValidation.h"
#include "cmdline/cmdline.h"
#include "VulkanDebug.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <functional>
#include <limits>

#if SDL_VERSION_ATLEAST(2, 0, 6)
#include <SDL_vulkan.h>
#endif

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace graphics {
namespace vulkan {

namespace {
#if SDL_SUPPORTS_VULKAN
const char* EngineName = "FreeSpaceOpen";

const gameversion::version MinVulkanVersion(1, 4, 0, 0);

VkBool32 VKAPI_PTR debugReportCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT /*messageTypes*/,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* /*pUserData*/)
{
	// Keep logging simple; add filtering if needed
	vkprintf("Vulkan message: [%d] %s\n", static_cast<int>(messageSeverity), pCallbackData->pMessage);
	return VK_FALSE;
}
#endif

const SCP_vector<const char*> RequiredDeviceExtensions = {
	VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
	VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
};

const SCP_vector<const char*> OptionalDeviceExtensions = {
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

bool checkDeviceExtensionSupport(PhysicalDeviceValues& values)
{
	values.extensions = values.device.enumerateDeviceExtensionProperties();

	std::set<std::string> requiredExtensions(RequiredDeviceExtensions.cbegin(), RequiredDeviceExtensions.cend());
	for (const auto& extension : values.extensions) {
		requiredExtensions.erase(extension.extensionName);
	}

	return requiredExtensions.empty();
}

bool checkSwapChainSupport(PhysicalDeviceValues& values, const vk::UniqueSurfaceKHR& surface)
{
	values.surfaceCapabilities = values.device.getSurfaceCapabilitiesKHR(surface.get());
	values.surfaceFormats = values.device.getSurfaceFormatsKHR(surface.get());
	values.presentModes = values.device.getSurfacePresentModesKHR(surface.get());

	return !values.surfaceFormats.empty() && !values.presentModes.empty();
}

bool isDeviceUnsuitable(PhysicalDeviceValues& values, const vk::UniqueSurfaceKHR& surface)
{
	// We need a GPU. Reject CPU or "other" types.
	if (values.properties.deviceType != vk::PhysicalDeviceType::eDiscreteGpu &&
		values.properties.deviceType != vk::PhysicalDeviceType::eIntegratedGpu &&
		values.properties.deviceType != vk::PhysicalDeviceType::eVirtualGpu) {
		vkprintf("Rejecting %s (%d) because the device type is unsuitable.\n",
			values.properties.deviceName.data(),
			values.properties.deviceID);
		return true;
	}

	uint32_t i = 0;
	for (const auto& queue : values.queueProperties) {
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
		vkprintf("Rejecting %s (%d) because the device does not have a graphics queue.\n",
			values.properties.deviceName.data(),
			values.properties.deviceID);
		return true;
	}
	if (!values.presentQueueIndex.initialized) {
		vkprintf("Rejecting %s (%d) because the device does not have a presentation queue.\n",
			values.properties.deviceName.data(),
			values.properties.deviceID);
		return true;
	}

	if (!checkDeviceExtensionSupport(values)) {
		vkprintf("Rejecting %s (%d) because the device does not support our required extensions.\n",
			values.properties.deviceName.data(),
			values.properties.deviceID);
		return true;
	}

	if (!checkSwapChainSupport(values, surface)) {
		vkprintf("Rejecting %s (%d) because the device swap chain was not compatible.\n",
			values.properties.deviceName.data(),
			values.properties.deviceID);
		return true;
	}

	if (values.properties.apiVersion < VK_API_VERSION_1_4) {
		vkprintf("Rejecting %s (%d) because device Vulkan version %d.%d.%d is below required %s.\n",
			values.properties.deviceName.data(),
			values.properties.deviceID,
			VK_VERSION_MAJOR(values.properties.apiVersion),
			VK_VERSION_MINOR(values.properties.apiVersion),
			VK_VERSION_PATCH(values.properties.apiVersion),
			gameversion::format_version(MinVulkanVersion).c_str());
		return true;
	}

	// Push descriptors are required for the Vulkan model path.
	if (!ValidatePushDescriptorSupport(values.features14)) {
		vkprintf("Rejecting %s (%d) because pushDescriptor feature is missing.\n",
			values.properties.deviceName.data(),
			values.properties.deviceID);
		return true;
	}

	// Descriptor indexing features are required for the Vulkan model path (bindless textures).
	if (!ValidateModelDescriptorIndexingSupport(values.features12)) {
		vkprintf("Rejecting %s (%d) because required descriptor indexing features are missing.\n",
			values.properties.deviceName.data(),
			values.properties.deviceID);
		return true;
	}

	// Dynamic rendering is required for the engine's renderPass-less pipelines.
	if (values.features13.dynamicRendering != VK_TRUE) {
		vkprintf("Rejecting %s (%d) because dynamicRendering feature is not supported.\n",
			values.properties.deviceName.data(),
			values.properties.deviceID);
		return true;
	}

	return false;
}

uint32_t deviceTypeScore(vk::PhysicalDeviceType type)
{
	switch (type) {
	case vk::PhysicalDeviceType::eIntegratedGpu:
		return 1;
	case vk::PhysicalDeviceType::eDiscreteGpu:
		return 2;
	case vk::PhysicalDeviceType::eVirtualGpu:
	case vk::PhysicalDeviceType::eCpu:
	case vk::PhysicalDeviceType::eOther:
	default:
		return 0;
	}
}

uint32_t scoreDevice(const PhysicalDeviceValues& device)
{
	uint32_t score = 0;

	score += deviceTypeScore(device.properties.deviceType) * 1000;
	score += device.properties.apiVersion * 100;

	return score;
}

bool compareDevices(const PhysicalDeviceValues& left, const PhysicalDeviceValues& right)
{
	return scoreDevice(left) < scoreDevice(right);
}

void printPhysicalDevice(const PhysicalDeviceValues& values)
{
	vkprintf("  Found %s (%d) of type %s. API version %d.%d.%d, Driver version %d.%d.%d. Scored as %d\n",
		values.properties.deviceName.data(),
		values.properties.deviceID,
		to_string(values.properties.deviceType).c_str(),
		VK_VERSION_MAJOR(values.properties.apiVersion),
		VK_VERSION_MINOR(values.properties.apiVersion),
		VK_VERSION_PATCH(values.properties.apiVersion),
		VK_VERSION_MAJOR(values.properties.driverVersion),
		VK_VERSION_MINOR(values.properties.driverVersion),
		VK_VERSION_PATCH(values.properties.driverVersion),
		scoreDevice(values));
}

vk::SurfaceFormatKHR chooseSurfaceFormat(const PhysicalDeviceValues& values)
{
	for (const auto& availableFormat : values.surfaceFormats) {
		// Simple check is enough for now
		if (availableFormat.format == vk::Format::eB8G8R8A8Srgb &&
			availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
			return availableFormat;
		}
	}

	return values.surfaceFormats.front();
}

vk::PresentModeKHR choosePresentMode(const PhysicalDeviceValues& values)
{
	// Depending on if we want Vsync or not, choose the best mode
	for (const auto& availablePresentMode : values.presentModes) {
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

vk::Extent2D chooseSwapChainExtent(const PhysicalDeviceValues& values, uint32_t width, uint32_t height)
{
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

} // namespace

VulkanRenderer::VulkanRenderer(std::unique_ptr<os::GraphicsOperations> graphicsOps)
	: m_graphicsOps(std::move(graphicsOps))
{
}
bool VulkanRenderer::initialize()
{
	vkprintf("Initializing Vulkan graphics device at %ix%i with %i-bit color...\n",
		gr_screen.max_w,
		gr_screen.max_h,
		gr_screen.bits_per_pixel);

	// Load the RenderDoc API if available before doing anything with OpenGL
	renderdoc::loadApi();

	if (!initDisplayDevice()) {
		return false;
	}

	if (!initializeInstance()) {
		vkprintf("Failed to create Vulkan instance!\n");
		return false;
	}

	if (!initializeSurface()) {
		vkprintf("Failed to create Vulkan surface!\n");
		return false;
	}

	PhysicalDeviceValues deviceValues;
	if (!pickPhysicalDevice(deviceValues)) {
		vkprintf("Could not find suitable physical Vulkan device.\n");
		return false;
	}

	if (!createLogicalDevice(deviceValues)) {
		vkprintf("Failed to create logical device.\n");
		return false;
	}

	if (!createSwapChain(deviceValues)) {
		vkprintf("Failed to create swap chain.\n");
		return false;
	}

	// Store device properties for later use
	m_deviceProperties = deviceValues.properties;
	m_memoryProperties = m_physicalDevice.getMemoryProperties();
	m_minUboAlignment = m_deviceProperties.limits.minUniformBufferOffsetAlignment;

	// Initialize vertex buffer alignment
	// Vulkan doesn't have a device property for vertex buffer alignment like uniform buffers,
	// but sizeof(float) is the minimum required alignment for vertex attributes.
	// This can be increased for better performance if needed (e.g., 16 bytes for vec4 alignment).
	m_vertexBufferAlignment = static_cast<uint32_t>(sizeof(float));

	// Query extended dynamic state 3 capabilities per-feature
	if (std::find_if(deviceValues.extensions.begin(),
			deviceValues.extensions.end(),
			[](const vk::ExtensionProperties& prop) {
				return std::strcmp(prop.extensionName.data(), VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) == 0;
			}) != deviceValues.extensions.end()) {
		m_supportsExtendedDynamicState3 = true;
		
		// Query per-capability support
		vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT extDyn3Feats{};
		vk::PhysicalDeviceFeatures2 feats2;
		feats2.pNext = &extDyn3Feats;
		m_physicalDevice.getFeatures2(&feats2);
		
		// Set capabilities based on what's actually supported
		m_extDyn3Caps.colorBlendEnable = extDyn3Feats.extendedDynamicState3ColorBlendEnable;
		m_extDyn3Caps.colorWriteMask = extDyn3Feats.extendedDynamicState3ColorWriteMask;
		m_extDyn3Caps.polygonMode = extDyn3Feats.extendedDynamicState3PolygonMode;
		m_extDyn3Caps.rasterizationSamples = extDyn3Feats.extendedDynamicState3RasterizationSamples;
	}

	// Prefer core vertex attribute divisor support (Vulkan 1.4 promotion).
	m_supportsVertexAttributeDivisor = m_deviceFeatures14.vertexAttributeInstanceRateDivisor;

	// If core support is absent, fall back to the extension (for forward compatibility/testing).
	if (!m_supportsVertexAttributeDivisor &&
		std::find_if(deviceValues.extensions.begin(),
			deviceValues.extensions.end(),
			[](const vk::ExtensionProperties& prop) {
				return std::strcmp(prop.extensionName.data(), VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0;
			}) != deviceValues.extensions.end()) {
		vk::PhysicalDeviceVertexAttributeDivisorFeaturesEXT divisorFeats{};
		vk::PhysicalDeviceFeatures2 feats2;
		feats2.pNext = &divisorFeats;
		m_physicalDevice.getFeatures2(&feats2);
		m_supportsVertexAttributeDivisor = divisorFeats.vertexAttributeInstanceRateDivisor;
	}

	createPipelineCache();
	createDescriptorResources();
	createDepthResources();
	createUploadCommandPool();
	createFrames();

	// Initialize managers
	const SCP_string shaderRoot = "code/graphics/shaders/compiled";
	m_shaderManager = std::make_unique<VulkanShaderManager>(m_device.get(), shaderRoot);

	m_pipelineManager = std::make_unique<VulkanPipelineManager>(m_device.get(),
		m_descriptorLayouts->pipelineLayout(),
		m_descriptorLayouts->modelPipelineLayout(),
		m_pipelineCache.get(),
		m_supportsExtendedDynamicState,
		m_supportsExtendedDynamicState2,
		m_supportsExtendedDynamicState3,
		m_extDyn3Caps,
		m_supportsVertexAttributeDivisor,
		m_deviceFeatures13.dynamicRendering == VK_TRUE);

	m_bufferManager = std::make_unique<VulkanBufferManager>(m_device.get(),
		m_memoryProperties,
		m_graphicsQueue,
		deviceValues.graphicsQueueIndex.index);

	m_textureManager = std::make_unique<VulkanTextureManager>(m_device.get(),
		m_memoryProperties,
		m_graphicsQueue,
		deviceValues.graphicsQueueIndex.index);

	m_graphicsQueueIndex = deviceValues.graphicsQueueIndex.index;
	m_presentQueueIndex = deviceValues.presentQueueIndex.index;

	return true;
}

bool VulkanRenderer::initDisplayDevice() const
{
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
bool VulkanRenderer::initializeInstance()
{
#if SDL_SUPPORTS_VULKAN
	const auto vkGetInstanceProcAddr =
		reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());

	VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

	const auto window = os::getSDLMainWindow();

	unsigned int count;
	if (!SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr)) {
		vkprintf("Error in first SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
		return false;
	}

	std::vector<const char*> extensions;
	extensions.resize(count);

	if (!SDL_Vulkan_GetInstanceExtensions(window, &count, extensions.data())) {
		vkprintf("Error in second SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
		return false;
	}

	const auto instanceVersion = vk::enumerateInstanceVersion();
	gameversion::version vulkanVersion(VK_VERSION_MAJOR(instanceVersion),
		VK_VERSION_MINOR(instanceVersion),
		VK_VERSION_PATCH(instanceVersion),
		0);
	vkprintf("Vulkan instance version %s\n", gameversion::format_version(vulkanVersion).c_str());

	if (vulkanVersion < MinVulkanVersion) {
		vkprintf("Vulkan version is less than the minimum which is %s.\n",
			gameversion::format_version(MinVulkanVersion).c_str());
		return false;
	}

	const auto supportedExtensions = vk::enumerateInstanceExtensionProperties();
	vkprintf("Instance extensions:\n");
	for (const auto& ext : supportedExtensions) {
		vkprintf("  Found support for %s version %" PRIu32 "\n", ext.extensionName.data(), ext.specVersion);
		if (FSO_DEBUG || Cmdline_graphics_debug_output) {
			if (!stricmp(ext.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
				extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			}
		}
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
		if (!stricmp(ext.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
			extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		}
#endif
	}

	std::vector<const char*> layers;
	const auto supportedLayers = vk::enumerateInstanceLayerProperties();
	vkprintf("Instance layers:\n");
	for (const auto& layer : supportedLayers) {
		vkprintf("  Found layer %s(%s). Spec version %d.%d.%d and implementation %" PRIu32 "\n",
			layer.layerName.data(),
			layer.description.data(),
			VK_VERSION_MAJOR(layer.specVersion),
			VK_VERSION_MINOR(layer.specVersion),
			VK_VERSION_PATCH(layer.specVersion),
			layer.implementationVersion);
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
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
	if (std::find_if(extensions.begin(), extensions.end(), [](const char* ext) {
			return !stricmp(ext, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		}) != extensions.end()) {
		createInfo.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
	}
#endif

	vk::DebugUtilsMessengerCreateInfoEXT createInstanceDebugInfo(
		{},
		vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
		vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
			vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
		debugReportCallback);

	vk::StructureChain<vk::InstanceCreateInfo, vk::DebugUtilsMessengerCreateInfoEXT> createInstanceChain(createInfo,
		createInstanceDebugInfo);

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

	m_vkInstance = std::move(instance);
	return true;
#else
	vkprintf("SDL does not support Vulkan in its current version.\n");
	return false;
#endif
}

bool VulkanRenderer::initializeSurface()
{
#if SDL_SUPPORTS_VULKAN
	const auto window = os::getSDLMainWindow();

	VkSurfaceKHR surface;
	if (!SDL_Vulkan_CreateSurface(window, static_cast<VkInstance>(*m_vkInstance), &surface)) {
		vkprintf("Failed to create vulkan surface: %s\n", SDL_GetError());
		return false;
	}

#if VK_HEADER_VERSION >= 301
	const vk::detail::ObjectDestroy<vk::Instance, VULKAN_HPP_DEFAULT_DISPATCHER_TYPE> deleter(*m_vkInstance,
#else
	const vk::ObjectDestroy<vk::Instance, VULKAN_HPP_DEFAULT_DISPATCHER_TYPE> deleter(*m_vkInstance,
#endif
		nullptr,
		VULKAN_HPP_DEFAULT_DISPATCHER);
	m_vkSurface = vk::UniqueSurfaceKHR(vk::SurfaceKHR(surface), deleter);
	return true;
#else
	return false;
#endif
}

bool VulkanRenderer::pickPhysicalDevice(PhysicalDeviceValues& deviceValues)
{
	const auto devices = m_vkInstance->enumeratePhysicalDevices();
	if (devices.empty()) {
		return false;
	}

	SCP_vector<PhysicalDeviceValues> values;
	std::transform(devices.cbegin(), devices.cend(), std::back_inserter(values), [](const vk::PhysicalDevice& dev) {
		PhysicalDeviceValues vals;
		vals.device = dev;

		vk::PhysicalDeviceProperties2 props;
		dev.getProperties2(&props);
		vals.properties = props.properties;

		vk::PhysicalDeviceFeatures2 feats;
		vals.features12 = vk::PhysicalDeviceVulkan12Features{};
		vals.features13 = vk::PhysicalDeviceVulkan13Features{};
		vals.features14 = vk::PhysicalDeviceVulkan14Features{};
		vals.extDynamicState = vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT{};
		vals.extDynamicState2 = vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT{};
		vals.extDynamicState3 = vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT{};
		vals.pushDescriptorProps = vk::PhysicalDevicePushDescriptorPropertiesKHR{};
		feats.pNext = &vals.features12;
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

	vkprintf("Physical Vulkan devices:\n");
	std::for_each(values.cbegin(), values.cend(), printPhysicalDevice);

	// Remove devices that do not have the features we need
	values.erase(std::remove_if(values.begin(),
					 values.end(),
					 [this](PhysicalDeviceValues& value) { return isDeviceUnsuitable(value, m_vkSurface); }),
		values.end());
	if (values.empty()) {
		return false;
	}

	// Sort the suitability of the devices in increasing order
	std::sort(values.begin(), values.end(), compareDevices);

	deviceValues = values.back();
	vkprintf("Selected device %s (%d) as the primary Vulkan device.\n",
		deviceValues.properties.deviceName.data(),
		deviceValues.properties.deviceID);
	vkprintf("Device extensions:\n");
	for (const auto& extProp : deviceValues.extensions) {
		vkprintf("  Found support for %s version %" PRIu32 "\n", extProp.extensionName.data(), extProp.specVersion);
	}

	return true;
}

bool VulkanRenderer::createLogicalDevice(const PhysicalDeviceValues& deviceValues)
{
	m_physicalDevice = deviceValues.device;

	float queuePriority = 1.0f;

	std::vector<vk::DeviceQueueCreateInfo> queueInfos;
	const std::set<uint32_t> familyIndices{deviceValues.graphicsQueueIndex.index,
	                                       deviceValues.presentQueueIndex.index};

	queueInfos.reserve(familyIndices.size());
	for (auto index : familyIndices) {
		queueInfos.emplace_back(vk::DeviceQueueCreateFlags(), index, 1, &queuePriority);
	}

	// Enable required/optional extensions
	std::vector<const char*> enabledExtensions(RequiredDeviceExtensions.begin(), RequiredDeviceExtensions.end());
	for (const auto& opt : OptionalDeviceExtensions) {
		auto it = std::find_if(deviceValues.extensions.begin(),
			deviceValues.extensions.end(),
			[&opt](const vk::ExtensionProperties& prop) { return std::strcmp(prop.extensionName.data(), opt) == 0; });
		if (it != deviceValues.extensions.end()) {
			enabledExtensions.push_back(opt);
		}
	}

	// Chain features for 1.2/1.3/1.4 and extension features
	// Create fresh structures to avoid copying stale pNext pointers from the query phase
	vk::PhysicalDeviceFeatures2 enabledFeatures;
	enabledFeatures.features = deviceValues.features;
	vk::PhysicalDeviceVulkan12Features enabled12 = deviceValues.features12;
	vk::PhysicalDeviceVulkan13Features enabled13 = deviceValues.features13;
	vk::PhysicalDeviceVulkan14Features enabled14 = deviceValues.features14;
	// Explicitly enable the features we depend on (after capability checks).
	enabled13.dynamicRendering = deviceValues.features13.dynamicRendering ? VK_TRUE : VK_FALSE;
	enabled14.vertexAttributeInstanceRateDivisor = deviceValues.features14.vertexAttributeInstanceRateDivisor;
	vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT enabledExtDyn = deviceValues.extDynamicState;
	vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT enabledExtDyn2 = deviceValues.extDynamicState2;
	vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT enabledExtDyn3 = deviceValues.extDynamicState3;
	// Explicitly reset pNext to ensure clean chain (copied structures may have stale pointers)
	enabled12.pNext = &enabled13;
	enabled13.pNext = &enabled14;
	enabled14.pNext = &enabledExtDyn;
	enabledExtDyn.pNext = &enabledExtDyn2;
	enabledExtDyn2.pNext = &enabledExtDyn3;
	enabledExtDyn3.pNext = nullptr;
	enabledFeatures.pNext = &enabled12;

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

	// Pipeline cache will be created in createPipelineCache()

	// Create queues
	m_graphicsQueue = m_device->getQueue(deviceValues.graphicsQueueIndex.index, 0);
	m_presentQueue = m_device->getQueue(deviceValues.presentQueueIndex.index, 0);

	// Keep sanitized copies of enabled Vulkan features for downstream validation.
	m_deviceFeatures13 = enabled13;
	m_deviceFeatures13.pNext = nullptr;

	// Keep a sanitized copy of the enabled Vulkan 1.4 features for downstream validation.
	m_deviceFeatures14 = enabled14;
	m_deviceFeatures14.pNext = nullptr;

	return true;
}
bool VulkanRenderer::createSwapChain(const PhysicalDeviceValues& deviceValues)
{
	// Choose one more than the minimum to avoid driver synchronization if it is not done with a thread yet
	uint32_t imageCount = deviceValues.surfaceCapabilities.minImageCount + 1;
	if (deviceValues.surfaceCapabilities.maxImageCount > 0 &&
		imageCount > deviceValues.surfaceCapabilities.maxImageCount) {
		imageCount = deviceValues.surfaceCapabilities.maxImageCount;
	}

	const auto surfaceFormat = chooseSurfaceFormat(deviceValues);

	vkprintf("Swapchain format=%s, colorSpace=%s\n",
		vk::to_string(surfaceFormat.format).c_str(),
		vk::to_string(surfaceFormat.colorSpace).c_str());

	vk::SwapchainCreateInfoKHR createInfo;
	createInfo.surface = m_vkSurface.get();
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = surfaceFormat.format;
	createInfo.imageColorSpace = surfaceFormat.colorSpace;
	createInfo.imageExtent = chooseSwapChainExtent(deviceValues, gr_screen.max_w, gr_screen.max_h);
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;

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

	m_swapChain = m_device->createSwapchainKHRUnique(createInfo);

	std::vector<vk::Image> swapChainImages = m_device->getSwapchainImagesKHR(m_swapChain.get());
	m_swapChainImages = SCP_vector<vk::Image>(swapChainImages.begin(), swapChainImages.end());
	m_swapChainImageFormat = surfaceFormat.format;
	m_swapChainExtent = createInfo.imageExtent;

	m_swapChainImageViews.reserve(m_swapChainImages.size());
	for (const auto& image : m_swapChainImages) {
		vk::ImageViewCreateInfo viewCreateInfo;
		viewCreateInfo.image = image;
		viewCreateInfo.viewType = vk::ImageViewType::e2D;
		viewCreateInfo.format = m_swapChainImageFormat;

		viewCreateInfo.components.r = vk::ComponentSwizzle::eIdentity;
		viewCreateInfo.components.g = vk::ComponentSwizzle::eIdentity;
		viewCreateInfo.components.b = vk::ComponentSwizzle::eIdentity;
		viewCreateInfo.components.a = vk::ComponentSwizzle::eIdentity;

		viewCreateInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		viewCreateInfo.subresourceRange.baseMipLevel = 0;
		viewCreateInfo.subresourceRange.levelCount = 1;
		viewCreateInfo.subresourceRange.baseArrayLayer = 0;
		viewCreateInfo.subresourceRange.layerCount = 1;

		m_swapChainImageViews.push_back(m_device->createImageViewUnique(viewCreateInfo));
	}

	return true;
}
void VulkanRenderer::createPipelineCache()
{
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
				const auto* header = reinterpret_cast<const CacheHeader*>(cacheData.data());
				if (header->headerLength == sizeof(CacheHeader) &&
					header->vendorID == m_deviceProperties.vendorID &&
					header->deviceID == m_deviceProperties.deviceID &&
					std::memcmp(header->pipelineCacheUUID, m_deviceProperties.pipelineCacheUUID, VK_UUID_SIZE) == 0) {
					validCache = true;
					// Skip header when creating cache
					vk::PipelineCacheCreateInfo cacheInfo;
					cacheInfo.initialDataSize = cacheData.size() - sizeof(CacheHeader);
					cacheInfo.pInitialData = cacheData.data() + sizeof(CacheHeader);
					m_pipelineCache = m_device->createPipelineCacheUnique(cacheInfo);
				} else {
					vkprintf("Vulkan: Pipeline cache UUID/driver mismatch, ignoring cache.\n");
				}
			}
		}
	}

	if (!validCache) {
		vk::PipelineCacheCreateInfo cacheInfo;
		m_pipelineCache = m_device->createPipelineCacheUnique(cacheInfo);
	}
}

void VulkanRenderer::createDescriptorResources()
{
	// Validate device limits before creating layouts - hard assert on failure
	VulkanDescriptorLayouts::validateDeviceLimits(m_deviceProperties.limits);
	EnsurePushDescriptorSupport(m_deviceFeatures14);

	m_descriptorLayouts = std::make_unique<VulkanDescriptorLayouts>(m_device.get());
	m_globalDescriptorSet = m_descriptorLayouts->allocateGlobalSet();
}

void VulkanRenderer::createFrames()
{
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		m_frames[i] = std::make_unique<VulkanFrame>(
			m_device.get(),
			m_graphicsQueueIndex,
			m_memoryProperties,
			UNIFORM_RING_SIZE,
			m_deviceProperties.limits.minUniformBufferOffsetAlignment,
			VERTEX_RING_SIZE,
			m_vertexBufferAlignment,
			STAGING_RING_SIZE,
			m_deviceProperties.limits.optimalBufferCopyOffsetAlignment);
	}
}

void VulkanRenderer::createDepthResources()
{
	m_depthFormat = findDepthFormat();

	vk::ImageCreateInfo imageInfo;
	imageInfo.imageType = vk::ImageType::e2D;
	imageInfo.format = m_depthFormat;
	imageInfo.extent = vk::Extent3D(m_swapChainExtent.width, m_swapChainExtent.height, 1);
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = vk::SampleCountFlagBits::e1;
	imageInfo.tiling = vk::ImageTiling::eOptimal;
	imageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	imageInfo.initialLayout = vk::ImageLayout::eUndefined;

	m_depthImage = m_device->createImageUnique(imageInfo);

	vk::MemoryRequirements memReqs = m_device->getImageMemoryRequirements(m_depthImage.get());
	vk::MemoryAllocateInfo allocInfo;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

	m_depthImageMemory = m_device->allocateMemoryUnique(allocInfo);
	m_device->bindImageMemory(m_depthImage.get(), m_depthImageMemory.get(), 0);

	vk::ImageViewCreateInfo viewInfo;
	viewInfo.image = m_depthImage.get();
	viewInfo.viewType = vk::ImageViewType::e2D;
	viewInfo.format = m_depthFormat;
	viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;

	m_depthImageView = m_device->createImageViewUnique(viewInfo);
}

vk::Format VulkanRenderer::findDepthFormat() const
{
	const std::vector<vk::Format> candidates = {
		vk::Format::eD32SfloatS8Uint,
		vk::Format::eD24UnormS8Uint,
		vk::Format::eD32Sfloat,
	};

	for (auto format : candidates) {
		vk::FormatProperties props = m_physicalDevice.getFormatProperties(format);
		if (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment) {
			return format;
		}
	}

	return vk::Format::eD32Sfloat; // Fallback
}

void VulkanRenderer::createUploadCommandPool()
{
	vk::CommandPoolCreateInfo poolInfo;
	poolInfo.queueFamilyIndex = m_graphicsQueueIndex;
	poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient;
	m_uploadCommandPool = m_device->createCommandPoolUnique(poolInfo);
}
uint32_t VulkanRenderer::acquireImage(VulkanFrame& frame)
{
	uint32_t imageIndex = std::numeric_limits<uint32_t>::max();
	auto res = m_device->acquireNextImageKHR(
		m_swapChain.get(), std::numeric_limits<uint64_t>::max(), frame.imageAvailable(), nullptr, &imageIndex);

	if (res == vk::Result::eErrorOutOfDateKHR || res == vk::Result::eSuboptimalKHR) {
		recreateSwapChain();
		return std::numeric_limits<uint32_t>::max();
	}

	if (res != vk::Result::eSuccess) {
		vkprintf("Failed to acquire swap chain image: %s\n", vk::to_string(res).c_str());
		return std::numeric_limits<uint32_t>::max();
	}

	return imageIndex;
}

void VulkanRenderer::beginFrame(VulkanFrame& frame, uint32_t imageIndex)
{
	m_frameLifecycle.begin(m_currentFrame);
	m_isRecording = true;

	// Sync model descriptors BEFORE command buffer recording
	// This is the ONLY place descriptors are updated for this frame
	Assertion(m_textureManager != nullptr, "m_textureManager must be initialized before beginFrame");

	// Only sync model descriptors if the model vertex heap exists
	vk::Buffer vertexHeapBuffer = queryModelVertexHeapBuffer();
	if (vertexHeapBuffer) {
		beginModelDescriptorSync(frame, m_currentFrame, vertexHeapBuffer);
	}

	vk::CommandBuffer cmd = frame.commandBuffer();

	vk::CommandBufferBeginInfo beginInfo;
	beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	cmd.begin(beginInfo);

	// Upload any pending textures before rendering begins (no render pass active yet)
	Assertion(m_textureManager != nullptr, "m_textureManager must be initialized before beginFrame");
	m_textureManager->flushPendingUploads(frame, cmd, m_currentFrame);

	// Transition swapchain image and depth to attachment layouts
	vk::ImageMemoryBarrier2 toRender{};
	toRender.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
	toRender.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
	toRender.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
	toRender.oldLayout = vk::ImageLayout::eUndefined;
	toRender.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
	toRender.image = m_swapChainImages[imageIndex];
	toRender.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	toRender.subresourceRange.levelCount = 1;
	toRender.subresourceRange.layerCount = 1;

	vk::ImageMemoryBarrier2 toDepth{};
	toDepth.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
	toDepth.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests;
	toDepth.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
		vk::AccessFlagBits2::eDepthStencilAttachmentRead;
	toDepth.oldLayout = m_depthImageInitialized ? vk::ImageLayout::eDepthAttachmentOptimal : vk::ImageLayout::eUndefined;
	toDepth.newLayout = vk::ImageLayout::eDepthAttachmentOptimal;
	toDepth.image = m_depthImage.get();
	toDepth.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
	toDepth.subresourceRange.levelCount = 1;
	toDepth.subresourceRange.layerCount = 1;

	std::array<vk::ImageMemoryBarrier2, 2> barriers = {toRender, toDepth};

	vk::DependencyInfo depInfo;
	depInfo.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
	depInfo.pImageMemoryBarriers = barriers.data();
	cmd.pipelineBarrier2(depInfo);
	m_depthImageInitialized = true;
	m_renderPassActive = false;
}

void VulkanRenderer::endFrame(VulkanFrame& frame, uint32_t imageIndex)
{
	if (!m_isRecording) {
		vkprintf("endFrame() - NOT RECORDING, skipping\n");
		return;
	}
	vk::CommandBuffer cmd = frame.commandBuffer();
	if (m_renderPassActive) {
		cmd.endRendering();
		m_renderPassActive = false;
	}

	// Transition to present
	vk::ImageMemoryBarrier2 toPresent{};
	toPresent.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
	toPresent.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
	toPresent.dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe;
	toPresent.dstAccessMask = {};
	toPresent.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
	toPresent.newLayout = vk::ImageLayout::ePresentSrcKHR;
	toPresent.image = m_swapChainImages[imageIndex];
	toPresent.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	toPresent.subresourceRange.levelCount = 1;
	toPresent.subresourceRange.layerCount = 1;

	vk::DependencyInfo depInfoPresent;
	depInfoPresent.imageMemoryBarrierCount = 1;
	depInfoPresent.pImageMemoryBarriers = &toPresent;
	cmd.pipelineBarrier2(depInfoPresent);

	cmd.end();
	m_frameLifecycle.end();
	m_isRecording = false;
}

void VulkanRenderer::submitFrame(VulkanFrame& frame, uint32_t imageIndex)
{
	vk::CommandBufferSubmitInfo cmdInfo;
	cmdInfo.commandBuffer = frame.commandBuffer();

	vk::SemaphoreSubmitInfo waitSemaphore;
	waitSemaphore.semaphore = frame.imageAvailable();
	waitSemaphore.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

	vk::SemaphoreSubmitInfo signalSemaphores[2];
	signalSemaphores[0].semaphore = frame.renderFinished();
	signalSemaphores[0].stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

	signalSemaphores[1].semaphore = frame.timelineSemaphore();
	signalSemaphores[1].value = frame.nextTimelineValue();
	signalSemaphores[1].stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

	vk::SubmitInfo2 submitInfo;
	submitInfo.waitSemaphoreInfoCount = 1;
	submitInfo.pWaitSemaphoreInfos = &waitSemaphore;
	submitInfo.commandBufferInfoCount = 1;
	submitInfo.pCommandBufferInfos = &cmdInfo;
	submitInfo.signalSemaphoreInfoCount = 2;
	submitInfo.pSignalSemaphoreInfos = signalSemaphores;

	m_graphicsQueue.submit2(submitInfo, frame.inflightFence());

	vk::PresentInfoKHR presentInfo;
	presentInfo.waitSemaphoreCount = 1;
	auto renderFinished = frame.renderFinished();
	presentInfo.pWaitSemaphores = &renderFinished;
	presentInfo.swapchainCount = 1;
	auto swapchain = m_swapChain.get();
	presentInfo.pSwapchains = &swapchain;
	presentInfo.pImageIndices = &imageIndex;

	auto presentResult = m_presentQueue.presentKHR(presentInfo);
	if (presentResult == vk::Result::eErrorOutOfDateKHR || presentResult == vk::Result::eSuboptimalKHR) {
		vkprintf("Swapchain out of date/suboptimal, recreating...\n");
		recreateSwapChain();
	} else if (presentResult != vk::Result::eSuccess) {
		vkprintf("ERROR - Failed to present swap chain image: %s\n", vk::to_string(presentResult).c_str());
	}

	frame.advanceTimeline();
}

void VulkanRenderer::incrementModelDraw()
{
	m_frameModelDraws++;
}

void VulkanRenderer::incrementPrimDraw()
{
	m_framePrimDraws++;
}

void VulkanRenderer::logFrameCounters()
{
	m_frameCounter++;
}

void VulkanRenderer::flip()
{
	// Finish previously recorded frame
	if (m_isRecording) {
		auto& recordingFrame = *m_frames[m_recordingFrame];
		endFrame(recordingFrame, m_recordingImage);
		submitFrame(recordingFrame, m_recordingImage);

		// Log per-frame draw counters now that the frame is finalized
		logFrameCounters();
		m_frameModelDraws = 0;
		m_framePrimDraws = 0;
	}

	// Advance frame index and prepare next frame
	m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
	auto& frame = *m_frames[m_currentFrame];
	frame.wait_for_gpu();

	// Process deferred buffer deletions
	Assertion(m_bufferManager != nullptr, "m_bufferManager must be initialized");
	m_bufferManager->onFrameEnd();
	Assertion(m_textureManager != nullptr, "m_textureManager must be initialized");
	m_textureManager->markUploadsCompleted(m_currentFrame);
	frame.reset();

	uint32_t imageIndex = acquireImage(frame);
	Assertion(imageIndex != std::numeric_limits<uint32_t>::max(), "flip() failed to acquire swapchain image");

	m_recordingFrame = m_currentFrame;
	m_recordingImage = imageIndex;
	beginFrame(frame, imageIndex);
}

VulkanFrame* VulkanRenderer::getCurrentRecordingFrame()
{
	if (!m_isRecording) {
		return nullptr;
	}
	return m_frames[m_recordingFrame].get();
}

void VulkanRenderer::ensureRenderingStarted(vk::CommandBuffer cmd)
{
	if (m_renderPassActive) {
		return;
	}

	const uint32_t imageIndex = m_recordingImage;

	// Begin dynamic rendering
	vk::RenderingAttachmentInfo colorAttachment{};
	colorAttachment.imageView = m_swapChainImageViews[imageIndex].get();
	colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	colorAttachment.loadOp = m_shouldClearColor ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
	
	colorAttachment.clearValue = vk::ClearColorValue(m_clearColor);

	vk::RenderingAttachmentInfo depthAttachment{};
	depthAttachment.imageView = m_depthImageView.get();
	depthAttachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
	depthAttachment.loadOp = m_shouldClearDepth ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
	depthAttachment.clearValue.depthStencil.depth = m_clearDepth;
	depthAttachment.clearValue.depthStencil.stencil = 0;

	vk::RenderingInfo renderingInfo{};
	renderingInfo.renderArea = vk::Rect2D({0, 0}, m_swapChainExtent);
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &colorAttachment;
	renderingInfo.pDepthAttachment = &depthAttachment;
	
	cmd.beginRendering(renderingInfo);
	m_renderPassActive = true;

	// Clear flags are one-shot; reset after we consume them
	m_shouldClearColor = false;
	m_shouldClearDepth = false;

	// Set dynamic state defaults
	// Vulkan Y-flip: set y=height and height=-height to match OpenGL coordinate system
	vk::Viewport viewport;
	viewport.x = 0.f;
	viewport.y = static_cast<float>(m_swapChainExtent.height);
	viewport.width = static_cast<float>(m_swapChainExtent.width);
	viewport.height = -static_cast<float>(m_swapChainExtent.height);
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;
	cmd.setViewport(0, viewport);

	vk::Rect2D scissor({0, 0}, m_swapChainExtent);
	cmd.setScissor(0, scissor);

	cmd.setCullMode(m_cullMode);
	cmd.setFrontFace(vk::FrontFace::eCounterClockwise);
	cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
	cmd.setDepthTestEnable(m_depthTestEnable ? VK_TRUE : VK_FALSE);
	cmd.setDepthWriteEnable(m_depthWriteEnable ? VK_TRUE : VK_FALSE);
	cmd.setDepthCompareOp(m_depthTestEnable ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eAlways);
	cmd.setStencilTestEnable(VK_FALSE);
	if (m_supportsExtendedDynamicState3) {
		if (m_extDyn3Caps.colorBlendEnable) {
			vk::Bool32 blendEnable = VK_FALSE;
			cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
		}
		if (m_extDyn3Caps.colorWriteMask) {
			vk::ColorComponentFlags mask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
				vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
			cmd.setColorWriteMaskEXT(0, vk::ArrayProxy<const vk::ColorComponentFlags>(1, &mask));
			vkprintf("Set color write mask dynamically: RGBA\n");
		} else {
			// If EDS3 colorWriteMask not available, rely on pipeline state
			// Pipeline should have color write mask set in buildBlendAttachment()
			static bool warnedOnce = false;
			if (!warnedOnce) {
				vkprintf("EDS3 colorWriteMask not available, using pipeline state\n");
				warnedOnce = true;
			}
		}
		if (m_extDyn3Caps.polygonMode) {
			cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
		}
		if (m_extDyn3Caps.rasterizationSamples) {
			cmd.setRasterizationSamplesEXT(vk::SampleCountFlagBits::e1);
		}
	}

	// Bind global descriptor set (no draw here; actual draws occur between flips)
	cmd.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 1, 1, &m_globalDescriptorSet, 0, nullptr);
}

vk::Buffer VulkanRenderer::getBuffer(gr_buffer_handle handle) const
{
	if (!m_bufferManager) {
		return nullptr;
	}
	return m_bufferManager->getBuffer(handle);
}

vk::Buffer VulkanRenderer::queryModelVertexHeapBuffer() const
{
	if (!m_modelVertexHeapHandle.isValid()) {
		return vk::Buffer{};
	}
	return getBuffer(m_modelVertexHeapHandle);
}

void VulkanRenderer::setModelVertexHeapHandle(gr_buffer_handle handle)
{
	m_modelVertexHeapHandle = handle;
	// Cache the raw vk::Buffer for legacy code paths that use getModelVertexHeapBuffer()
	m_modelVertexHeapBuffer = getBuffer(handle);
}

gr_buffer_handle VulkanRenderer::createBuffer(BufferType type, BufferUsageHint usage)
{
	if (!m_bufferManager) {
		return gr_buffer_handle::invalid();
	}
	return m_bufferManager->createBuffer(type, usage);
}

void VulkanRenderer::deleteBuffer(gr_buffer_handle handle)
{
	if (m_bufferManager) {
		m_bufferManager->deleteBuffer(handle);
	}
}

void VulkanRenderer::updateBufferData(gr_buffer_handle handle, size_t size, const void* data)
{
	if (m_bufferManager) {
		m_bufferManager->updateBufferData(handle, size, data);
	}
}

void VulkanRenderer::updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void* data)
{
	if (m_bufferManager) {
		m_bufferManager->updateBufferDataOffset(handle, offset, size, data);
	}
}

void* VulkanRenderer::mapBuffer(gr_buffer_handle handle)
{
	if (!m_bufferManager) {
		return nullptr;
	}
	return m_bufferManager->mapBuffer(handle);
}

void VulkanRenderer::flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size)
{
	if (m_bufferManager) {
		m_bufferManager->flushMappedBuffer(handle, offset, size);
	}
}

void VulkanRenderer::resizeBuffer(gr_buffer_handle handle, size_t size)
{
	if (m_bufferManager) {
		m_bufferManager->resizeBuffer(handle, size);
	}
}

vk::DescriptorImageInfo VulkanRenderer::getTextureDescriptor(int bitmapHandle,
	VulkanFrame& frame,
	vk::CommandBuffer cmd,
	const VulkanTextureManager::SamplerKey& samplerKey)
{
	if (!m_textureManager) {
		throw std::runtime_error("Vulkan texture manager not initialized; cannot supply texture descriptor");
	}

	try {
		return m_textureManager->getDescriptor(
			bitmapHandle,
			frame,
			cmd,
			getCurrentFrameIndex(),
			m_renderPassActive,
			samplerKey);
	} catch (const std::runtime_error&) {
		// If a texture is requested mid-render-pass and not resident, end the pass, upload, and restart.
		if (!m_renderPassActive) {
			throw;
		}

		cmd.endRendering();
		m_renderPassActive = false;

		m_textureManager->flushPendingUploads(frame, cmd, getCurrentFrameIndex());

		auto descriptor = m_textureManager->getDescriptor(
			bitmapHandle,
			frame,
			cmd,
			getCurrentFrameIndex(),
			/*renderPassActive=*/false,
			samplerKey);

		// Resume rendering for the caller; dynamic state will be reset by ensureRenderingStarted.
		ensureRenderingStarted(cmd);
		return descriptor;
	}
}

void VulkanRenderer::setModelUniformBinding(VulkanFrame& frame,
	gr_buffer_handle handle,
	size_t offset,
	size_t size)
{
	const auto alignment = getMinUniformOffsetAlignment();
	Assertion(offset <= std::numeric_limits<uint32_t>::max(),
		"Model uniform offset %zu exceeds uint32_t range", offset);
	const auto dynOffset = static_cast<uint32_t>(offset);

	Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
	Assertion((dynOffset % alignment) == 0,
		"Model uniform offset %u is not aligned to %zu", dynOffset, alignment);
	Assertion(size >= sizeof(model_uniform_data),
		"Model uniform size %zu is smaller than sizeof(model_uniform_data) %zu",
		size, sizeof(model_uniform_data));

	Assertion(frame.modelDescriptorSet, "Model descriptor set must be allocated before binding uniform buffer");
	Assertion(handle.isValid(), "Invalid model uniform buffer handle");

	if (frame.modelUniformState.bufferHandle != handle) {
		vk::Buffer vkBuffer = getBuffer(handle);
		Assertion(vkBuffer, "Failed to resolve Vulkan buffer for handle %d", handle.value());

		vk::DescriptorBufferInfo info{};
		info.buffer = vkBuffer;
		info.offset = 0;
		info.range = sizeof(model_uniform_data);

		vk::WriteDescriptorSet write{};
		write.dstSet = frame.modelDescriptorSet;
		write.dstBinding = 2;
		write.dstArrayElement = 0;
		write.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
		write.descriptorCount = 1;
		write.pBufferInfo = &info;

		m_device->updateDescriptorSets(1, &write, 0, nullptr);

		frame.modelUniformState.bufferHandle = handle;
	}

	frame.modelUniformState.dynamicOffset = dynOffset;
}

void VulkanRenderer::updateModelDescriptors(vk::DescriptorSet set,
	vk::Buffer vertexBuffer,
	vk::DeviceSize vertexOffset,
	vk::DeviceSize vertexRange,
	const std::vector<std::pair<uint32_t, int>>& textures,
	VulkanFrame& frame,
	vk::CommandBuffer cmd)
{
	std::vector<vk::WriteDescriptorSet> writes;
	writes.reserve(textures.size() + 1);

	// Vertex buffer (binding 0)
	vk::DescriptorBufferInfo bufferInfo{vertexBuffer, vertexOffset, vertexRange};
	writes.push_back({set, 0, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &bufferInfo, nullptr});

	// Textures (binding 1)
	std::vector<vk::DescriptorImageInfo> imageInfos;
	imageInfos.reserve(textures.size());
	for (const auto& [arrayIndex, handle] : textures) {
		auto samplerKey = VulkanTextureManager::SamplerKey{};
		// Use default sampler settings for now; can be extended later
		samplerKey.address = vk::SamplerAddressMode::eRepeat;
		samplerKey.filter = vk::Filter::eLinear;

		vk::DescriptorImageInfo info = getTextureDescriptor(handle, frame, cmd, samplerKey);
		imageInfos.push_back(info);
		writes.push_back({set, 1, arrayIndex, 1, vk::DescriptorType::eCombinedImageSampler,
			&imageInfos.back(), nullptr, nullptr});
	}

	m_device->updateDescriptorSets(writes, {});
}

void VulkanRenderer::beginModelDescriptorSync(VulkanFrame& frame, uint32_t frameIndex, vk::Buffer vertexHeapBuffer)
{
	// Precondition: vertexHeapBuffer is valid (caller checked)
	Assertion(static_cast<VkBuffer>(vertexHeapBuffer) != VK_NULL_HANDLE,
	          "beginModelDescriptorSync called with null vertexHeapBuffer");

	// frameIndex MUST be ring index [0, FramesInFlight)
	Assertion(frameIndex < kFramesInFlight,
	          "Invalid frame index %u (must be 0..%u)", frameIndex, kFramesInFlight - 1);

	// Allocate descriptor set if needed
	if (!frame.modelDescriptorSet) {
		frame.modelDescriptorSet = m_descriptorLayouts->allocateModelDescriptorSet();
		Assertion(frame.modelDescriptorSet, "Failed to allocate model descriptor set");
	}

	// Binding 0: Write vertex heap descriptor (once per frame)
	writeVertexHeapDescriptor(frame, vertexHeapBuffer);

	// Binding 1: Write all dirty texture descriptors for RESIDENT textures
	for (auto& [handle, record] : m_textureManager->allTextures()) {
		if (record.state != VulkanTextureManager::TextureState::Resident) {
			continue;
		}

		auto& state = record.bindingState;
		if (!state.descriptorWritten[frameIndex]) {
			writeTextureDescriptor(frame.modelDescriptorSet, state.arrayIndex, handle);
			state.descriptorWritten[frameIndex] = true;
		}
	}

	// Write fallback descriptor into retired slots
	// This ensures any stale references sample black instead of destroyed image
	for (uint32_t slot : m_textureManager->getRetiredSlots()) {
		writeFallbackDescriptor(frame.modelDescriptorSet, slot);
	}

	// Clear retired slots once all frames have been updated
	m_textureManager->clearRetiredSlotsIfAllFramesUpdated(frameIndex);
}

void VulkanRenderer::writeVertexHeapDescriptor(VulkanFrame& frame, vk::Buffer vertexHeapBuffer)
{
	// Precondition: vertexHeapBuffer is valid (caller checked)
	Assertion(static_cast<VkBuffer>(vertexHeapBuffer) != VK_NULL_HANDLE,
	          "writeVertexHeapDescriptor called with null vertexHeapBuffer");

	vk::DescriptorBufferInfo info;
	info.buffer = vertexHeapBuffer;
	info.offset = 0;
	info.range = VK_WHOLE_SIZE;

	vk::WriteDescriptorSet write;
	write.dstSet = frame.modelDescriptorSet;
	write.dstBinding = 0;
	write.dstArrayElement = 0;
	write.descriptorCount = 1;
	write.descriptorType = vk::DescriptorType::eStorageBuffer;
	write.pBufferInfo = &info;

	m_device->updateDescriptorSets(1, &write, 0, nullptr);
}

void VulkanRenderer::writeTextureDescriptor(vk::DescriptorSet set,
	uint32_t arrayIndex,
	int textureHandle)
{
	Assertion(arrayIndex < kMaxBindlessTextures,
	          "Texture array index %u out of bounds", arrayIndex);

	VulkanTextureManager::SamplerKey samplerKey{};
	samplerKey.address = vk::SamplerAddressMode::eRepeat;
	samplerKey.filter = vk::Filter::eLinear;

	vk::DescriptorImageInfo info = m_textureManager->getTextureDescriptorInfo(
		textureHandle, samplerKey);

	Assertion(info.imageView, "Texture %d must be resident when writing descriptor", textureHandle);

	vk::WriteDescriptorSet write;
	write.dstSet = set;
	write.dstBinding = 1;
	write.dstArrayElement = arrayIndex;
	write.descriptorCount = 1;
	write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo = &info;

	m_device->updateDescriptorSets(1, &write, 0, nullptr);
}

void VulkanRenderer::writeFallbackDescriptor(vk::DescriptorSet set, uint32_t arrayIndex)
{
	Assertion(arrayIndex < kMaxBindlessTextures,
	          "Fallback slot %u out of bounds", arrayIndex);

	// Use the fallback texture (black 1x1, initialized at startup)
	int fallbackHandle = m_textureManager->getFallbackTextureHandle();
	Assertion(fallbackHandle >= 0, "Fallback texture must be initialized");

	VulkanTextureManager::SamplerKey samplerKey{};
	samplerKey.address = vk::SamplerAddressMode::eRepeat;
	samplerKey.filter = vk::Filter::eNearest;

	vk::DescriptorImageInfo info = m_textureManager->getTextureDescriptorInfo(
		fallbackHandle, samplerKey);

	Assertion(info.imageView, "Fallback texture must be resident");

	vk::WriteDescriptorSet write;
	write.dstSet = set;
	write.dstBinding = 1;
	write.dstArrayElement = arrayIndex; // THE ORIGINAL SLOT, not 0
	write.descriptorCount = 1;
	write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo = &info;

	m_device->updateDescriptorSets(1, &write, 0, nullptr);
}

int VulkanRenderer::preloadTexture(int bitmapHandle, bool isAABitmap)
{
	if (m_textureManager && bitmapHandle >= 0) {
		return m_textureManager->preloadTexture(bitmapHandle, isAABitmap) ? 1 : 0;
	}
	return 0;
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const
{
	for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i) {
		if ((typeFilter & (1 << i)) &&
			(m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}
	throw std::runtime_error("Failed to find suitable memory type.");
}

void VulkanRenderer::immediateSubmit(const std::function<void(vk::CommandBuffer)>& recorder)
{
	vk::CommandBufferAllocateInfo allocInfo;
	allocInfo.level = vk::CommandBufferLevel::ePrimary;
	allocInfo.commandPool = m_uploadCommandPool.get();
	allocInfo.commandBufferCount = 1;

	auto cmdBuffers = m_device->allocateCommandBuffersUnique(allocInfo);
	auto& cmdBuffer = cmdBuffers[0];

	vk::CommandBufferBeginInfo beginInfo;
	beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	cmdBuffer->begin(beginInfo);

	recorder(cmdBuffer.get());

	cmdBuffer->end();

	vk::SubmitInfo submitInfo;
	submitInfo.commandBufferCount = 1;
	auto cmdBufferHandle = cmdBuffer.get();
	submitInfo.pCommandBuffers = &cmdBufferHandle;

	m_graphicsQueue.submit(submitInfo, nullptr);
	m_graphicsQueue.waitIdle();
}

bool VulkanRenderer::recreateSwapChain()
{
	m_device->waitIdle();

	// Cleanup old swapchain resources
	m_swapChainImageViews.clear();

	// Recreate swapchain (simplified - would need to query new surface capabilities)
	// For now, just return false to indicate failure
	return false;
}

void VulkanRenderer::shutdown()
{
	if (!m_device) {
		return; // Already shut down or never initialized
	}

	m_device->waitIdle();

	// Save pipeline cache while device is still valid
	if (m_pipelineCache) {
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
				header.vendorID = m_deviceProperties.vendorID;
				header.deviceID = m_deviceProperties.deviceID;
				std::memcpy(header.pipelineCacheUUID, m_deviceProperties.pipelineCacheUUID, VK_UUID_SIZE);
				cacheFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
				cacheFile.write(reinterpret_cast<const char*>(cacheData.data()), cacheData.size());
			}
		}
	}

	// Clear non-owned handles (swapchain images are owned by the swapchain, not us)
	m_swapChainImages.clear();

	// All RAII members (vk::UniqueXXX, std::unique_ptr) are cleaned up by the
	// destructor in reverse declaration order, which respects Vulkan's destruction
	// dependencies:
	//   - Device-dependent resources destroyed before device
	//   - Surface and debug messenger destroyed before instance
}

void VulkanRenderer::setClearColor(int r, int g, int b)
{
	m_clearColor[0] = r / 255.0f;
	m_clearColor[1] = g / 255.0f;
	m_clearColor[2] = b / 255.0f;
	m_clearColor[3] = 1.0f;
}

int VulkanRenderer::setCullMode(int cull)
{
	switch (cull) {
	case 0:
		m_cullMode = vk::CullModeFlagBits::eNone;
		break;
	case 1:
		m_cullMode = vk::CullModeFlagBits::eBack;
		break;
	case 2:
		m_cullMode = vk::CullModeFlagBits::eFront;
		break;
	default:
		return 0;
	}
	return 1;
}

int VulkanRenderer::setZbufferMode(int mode)
{
	switch (mode) {
	case 0: // ZBUFFER_TYPE_NONE
		m_depthTestEnable = false;
		m_depthWriteEnable = false;
		m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_NONE;
		break;
	case 1: // ZBUFFER_TYPE_READ
		m_depthTestEnable = true;
		m_depthWriteEnable = false;
		m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_READ;
		break;
	case 2: // ZBUFFER_TYPE_WRITE
		m_depthTestEnable = false;
		m_depthWriteEnable = true;
		m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_WRITE;
		break;
	case 3: // ZBUFFER_TYPE_FULL
		m_depthTestEnable = true;
		m_depthWriteEnable = true;
		m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_FULL;
		break;
	default:
		return 0;
	}
	return 1;
}

int VulkanRenderer::getZbufferMode() const
{
	return static_cast<int>(m_zbufferMode);
}

void VulkanRenderer::requestClear()
{
	m_shouldClearColor = true;
	m_shouldClearDepth = true;
}

} // namespace vulkan
} // namespace graphics
