#pragma once

#include "VulkanDevice.h"
#include "VulkanRenderTargets.h"
#include "VulkanDescriptorLayouts.h"

#include <array>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

enum class RenderMode {
	Swapchain,        // Forward rendering to swapchain
	DeferredGBuffer   // Deferred geometry pass to G-buffer
};

class VulkanRenderingSession {
  public:
	VulkanRenderingSession(VulkanDevice& device,
		VulkanRenderTargets& targets,
		VulkanDescriptorLayouts& descriptorLayouts);

	// Frame boundaries - called by VulkanRenderer
	void beginFrame(vk::CommandBuffer cmd, uint32_t imageIndex, vk::DescriptorSet globalDescriptorSet);
	void endFrame(vk::CommandBuffer cmd, uint32_t imageIndex);

	// Render pass management
	void ensureRenderingActive(vk::CommandBuffer cmd, uint32_t imageIndex);
	void endRendering(vk::CommandBuffer cmd);
	bool isRenderPassActive() const { return m_renderPassActive; }

	// Mode switching
	void setMode(RenderMode mode) { m_pendingMode = mode; }
	RenderMode activeMode() const { return m_activeMode; }
	RenderMode pendingMode() const { return m_pendingMode; }

	// Deferred rendering
	void beginDeferredPass(bool clearNonColorBufs);
	void endDeferredGeometry(vk::CommandBuffer cmd);
	bool isDeferredActive() const { return m_deferredActive; }
	bool isDeferredGeometryDone() const { return m_deferredGeometryDone; }
	void setPendingModeSwapchain() { m_pendingMode = RenderMode::Swapchain; }

	// Clear control
	void requestClear();
	void setClearColor(float r, float g, float b, float a);

	// State setters
	void setCullMode(vk::CullModeFlagBits mode) { m_cullMode = mode; }
	void setDepthTest(bool enable) { m_depthTest = enable; }
	void setDepthWrite(bool enable) { m_depthWrite = enable; }

	// State getters
	vk::CullModeFlagBits cullMode() const { return m_cullMode; }
	bool depthTestEnabled() const { return m_depthTest; }
	bool depthWriteEnabled() const { return m_depthWrite; }

	// Queries for pipeline creation
	VkFormat currentColorFormat() const;
	uint32_t currentColorAttachmentCount() const;

	// Reset for new frame
	void resetFrameState();

	// Dynamic state application (public - called by VulkanRenderer)
	void applyDynamicState(vk::CommandBuffer cmd, vk::DescriptorSet globalDescriptorSet);

  private:
	// Render pass variants
	void beginSwapchainRendering(vk::CommandBuffer cmd, uint32_t imageIndex);
	void beginGBufferRendering(vk::CommandBuffer cmd);

	// Layout transitions (barriers encapsulated here)
	void transitionSwapchainToAttachment(vk::CommandBuffer cmd, uint32_t imageIndex);
	void transitionDepthToAttachment(vk::CommandBuffer cmd);
	void transitionSwapchainToPresent(vk::CommandBuffer cmd, uint32_t imageIndex);
	void transitionGBufferToAttachment(vk::CommandBuffer cmd);
	void transitionGBufferToShaderRead(vk::CommandBuffer cmd);

	VulkanDevice& m_device;
	VulkanRenderTargets& m_targets;
	VulkanDescriptorLayouts& m_descriptorLayouts;

	// Render pass state
	RenderMode m_activeMode = RenderMode::Swapchain;
	RenderMode m_pendingMode = RenderMode::Swapchain;
	bool m_renderPassActive = false;

	// Deferred state
	bool m_deferredActive = false;
	bool m_deferredGeometryDone = false;
	bool m_deferredClearNonColorBufs = false;

	// Clear state
	std::array<float, 4> m_clearColor{0.f, 0.f, 0.f, 1.f};
	float m_clearDepth = 1.0f;
	bool m_shouldClearColor = true;
	bool m_shouldClearDepth = true;

	// Dynamic state cache
	vk::CullModeFlagBits m_cullMode = vk::CullModeFlagBits::eBack;
	bool m_depthTest = true;
	bool m_depthWrite = true;

	// Current frame's image index for barrier targets
	uint32_t m_currentImageIndex = 0;
};

} // namespace vulkan
} // namespace graphics
