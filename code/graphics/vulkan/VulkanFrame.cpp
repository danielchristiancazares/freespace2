#include "VulkanFrame.h"
#include "VulkanDebug.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace graphics {
namespace vulkan {

VulkanFrame::VulkanFrame(vk::Device device,
	uint32_t queueFamilyIndex,
	const vk::PhysicalDeviceMemoryProperties& memoryProps,
	vk::DeviceSize uniformBufferSize,
	vk::DeviceSize uniformAlignment,
	vk::DeviceSize vertexBufferSize,
	vk::DeviceSize vertexAlignment,
	vk::DeviceSize stagingBufferSize,
	vk::DeviceSize stagingAlignment)
	: m_device(device),
	  m_uniformRing(device, memoryProps, uniformBufferSize, uniformAlignment,
	                vk::BufferUsageFlagBits::eUniformBuffer),
	  m_vertexRing(device, memoryProps, vertexBufferSize, vertexAlignment,
	               vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer),
	  m_stagingRing(device, memoryProps, stagingBufferSize,
	                stagingAlignment == 0 ? 1 : stagingAlignment,
	                vk::BufferUsageFlagBits::eTransferSrc)
{
	vk::CommandPoolCreateInfo poolInfo;
	poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	poolInfo.queueFamilyIndex = queueFamilyIndex;
	m_commandPool = m_device.createCommandPoolUnique(poolInfo);

	vk::CommandBufferAllocateInfo cmdAlloc;
	cmdAlloc.commandPool = m_commandPool.get();
	cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
	cmdAlloc.commandBufferCount = 1;
	m_commandBuffer = m_device.allocateCommandBuffers(cmdAlloc).front();

	vk::FenceCreateInfo fenceInfo;
	fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled; // allow first frame without waiting
	m_inflightFence = m_device.createFenceUnique(fenceInfo);

	vk::SemaphoreTypeCreateInfo timelineType;
	timelineType.semaphoreType = vk::SemaphoreType::eTimeline;
	timelineType.initialValue = m_timelineValue;

	vk::SemaphoreCreateInfo semaphoreInfo;
	semaphoreInfo.pNext = &timelineType;
	m_timelineSemaphore = m_device.createSemaphoreUnique(semaphoreInfo);

	vk::SemaphoreCreateInfo binaryInfo;
	m_imageAvailable = m_device.createSemaphoreUnique(binaryInfo);
	m_renderFinished = m_device.createSemaphoreUnique(binaryInfo);
}

void VulkanFrame::wait_for_gpu()
{
	// #region agent log
	auto agent_log = [](const char* hypothesisId, const char* location, const char* message, auto&& dataWriter) {
		std::ofstream logFile("c:\\Users\\danie\\Documents\\freespace2\\.cursor\\debug.log", std::ios::app);
		const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\""
		        << hypothesisId << "\",\"location\":\"" << location << "\",\"message\":\""
		        << message << "\",\"data\":";
		dataWriter(logFile);
		logFile << ",\"timestamp\":" << now << "}\n";
	};
	// #endregion

	auto fence = m_inflightFence.get();
	// #region agent log
	agent_log("H1", "VulkanFrame.cpp:wait_for_gpu", "entry",
		[this, fence](std::ofstream& out) {
			auto rawFence = static_cast<VkFence>(fence);
			out << "{\"fenceHandle\":" << static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rawFence))
			    << ",\"lastSubmitSerial\":" << m_lastSubmitSerial
			    << ",\"lastSubmitFrameIndex\":" << m_lastSubmitFrameIndex
			    << ",\"lastSubmitImageIndex\":" << m_lastSubmitImageIndex
			    << ",\"lastSubmitTimeline\":" << m_lastSubmitTimeline
			    << ",\"hasSubmitInfo\":" << (m_hasSubmitInfo ? 1 : 0)
			    << "}";
		});
	// #endregion
	if (!fence) {
		vkprintf("VulkanFrame::wait_for_gpu called with null fence\n");
		// #region agent log
		agent_log("H1", "VulkanFrame.cpp:wait_for_gpu", "null fence branch",
			[](std::ofstream& out) { out << "{}"; });
		// #endregion
		return;
	}

	auto result = m_device.waitForFences(1, &fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
	// #region agent log
	agent_log("H2", "VulkanFrame.cpp:wait_for_gpu", "waitForFences completed",
		[this, result](std::ofstream& out) {
			out << "{\"result\":" << static_cast<int>(result)
			    << ",\"resultString\":\"" << vk::to_string(result) << "\""
			    << ",\"isDeviceLost\":" << (result == vk::Result::eErrorDeviceLost ? 1 : 0)
			    << ",\"isTimeout\":" << (result == vk::Result::eTimeout ? 1 : 0)
			    << ",\"hasSubmitInfo\":" << (m_hasSubmitInfo ? 1 : 0)
			    << ",\"lastSubmitSerial\":" << m_lastSubmitSerial
			    << "}";
		});
	// #endregion
	if (result != vk::Result::eSuccess) {
		vkprintf("VulkanFrame::wait_for_gpu: waitForFences returned %d (%s)\n", static_cast<int>(result), vk::to_string(result).c_str());
		// #region agent log
		agent_log("H2", "VulkanFrame.cpp:wait_for_gpu", "waitForFences failed",
			[this, result](std::ofstream& out) {
				out << "{\"result\":" << static_cast<int>(result)
				    << ",\"resultString\":\"" << vk::to_string(result) << "\""
				    << ",\"isDeviceLost\":" << (result == vk::Result::eErrorDeviceLost ? 1 : 0)
				    << ",\"hasSubmitInfo\":" << (m_hasSubmitInfo ? 1 : 0)
				    << "}";
			});
		// #endregion
		Assertion(false, "Fence wait failed for Vulkan frame");
		throw std::runtime_error("Fence wait failed for Vulkan frame");
	}
	auto resetResult = m_device.resetFences(1, &fence);
	// #region agent log
	agent_log("H3", "VulkanFrame.cpp:wait_for_gpu", "resetFences completed",
		[resetResult](std::ofstream& out) { out << "{\"resetResult\":" << static_cast<int>(resetResult) << "}"; });
	// #endregion
	if (resetResult != vk::Result::eSuccess) {
		vkprintf("VulkanFrame::wait_for_gpu: resetFences returned %d\n", static_cast<int>(resetResult));
		Assertion(false, "Failed to reset fence for Vulkan frame");
		throw std::runtime_error("Failed to reset fence for Vulkan frame");
	}
	// #region agent log
	agent_log("H4", "VulkanFrame.cpp:wait_for_gpu", "exit success",
		[](std::ofstream& out) { out << "{}"; });
	// #endregion
}

void VulkanFrame::record_submit_info(uint32_t frameIndex, uint32_t imageIndex, uint64_t timelineValue, uint64_t submitSerial)
{
	m_lastSubmitFrameIndex = frameIndex;
	m_lastSubmitImageIndex = imageIndex;
	m_lastSubmitTimeline = timelineValue;
	m_lastSubmitSerial = submitSerial;
	m_hasSubmitInfo = true;
}

void VulkanFrame::reset()
{
	using ResetReturn = decltype(m_device.resetCommandPool(m_commandPool.get()));
	constexpr bool returnsVoid = std::is_same_v<ResetReturn, void>;

	auto reset_pool = [](auto& dev, VkCommandPool pool, std::true_type) {
		dev.resetCommandPool(pool);          // returns void
		return vk::Result::eSuccess;
	};
	auto reset_pool_result = [](auto& dev, VkCommandPool pool, std::false_type) {
		return dev.resetCommandPool(pool);   // returns vk::Result
	};

	vk::Result poolResult = reset_pool(m_device, m_commandPool.get(), std::integral_constant<bool, returnsVoid>{});
	if (poolResult != vk::Result::eSuccess) {
		throw std::runtime_error("Failed to reset command pool for Vulkan frame");
	}
	m_uniformRing.reset();
	m_vertexRing.reset();
	m_stagingRing.reset();
}

} // namespace vulkan
} // namespace graphics
