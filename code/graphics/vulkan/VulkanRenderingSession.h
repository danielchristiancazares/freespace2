#pragma once

#include "VulkanDevice.h"
#include "VulkanRenderTargets.h"
#include "VulkanRenderTargetInfo.h"

#include <vulkan/vulkan.hpp>
#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace graphics {
namespace vulkan {

class VulkanRenderingSession {
public:
  VulkanRenderingSession(VulkanDevice& device,
    VulkanRenderTargets& targets);

    // Frame boundaries - called by VulkanRenderer
    void beginFrame(vk::CommandBuffer cmd, uint32_t imageIndex);
    void endFrame(vk::CommandBuffer cmd, uint32_t imageIndex);

  // Starts dynamic rendering for the *current target* if not already active.
  // Returns the render target contract used for pipeline selection.
  RenderTargetInfo ensureRendering(vk::CommandBuffer cmd, uint32_t imageIndex);
  bool renderingActive() const { return m_activePass.has_value(); }

  // Boundary-facing state transitions (no "pending", no dual state)
  void requestSwapchainTarget();                  // swapchain + depth
  void beginDeferredPass(bool clearNonColorBufs); // selects gbuffer target
  void endDeferredGeometry(vk::CommandBuffer cmd);// transitions gbuffer -> shader read, selects swapchain-no-depth

  // Clear control
  void requestClear();
  void requestDepthClear();
  void setClearColor(float r, float g, float b, float a);

  // State setters
  void setCullMode(vk::CullModeFlagBits mode) { m_cullMode = mode; }
  void setDepthTest(bool enable) { m_depthTest = enable; }
  void setDepthWrite(bool enable) { m_depthWrite = enable; }

  // State getters
  vk::CullModeFlagBits cullMode() const { return m_cullMode; }
  bool depthTestEnabled() const { return m_depthTest; }
  bool depthWriteEnabled() const { return m_depthWrite; }

  // Dynamic state application (public - called by VulkanRenderer)
  void applyDynamicState(vk::CommandBuffer cmd);

private:
  // RAII guard for active dynamic rendering - destruction records endRendering().
  struct ActivePass {
    vk::CommandBuffer cmd{};
    explicit ActivePass(vk::CommandBuffer c) : cmd(c) {}
    ~ActivePass()
    {
      if (cmd) {
        cmd.endRendering();
      }
    }

    ActivePass(const ActivePass&) = delete;
    ActivePass& operator=(const ActivePass&) = delete;
    ActivePass(ActivePass&& other) noexcept : cmd(other.cmd)
    {
      other.cmd = vk::CommandBuffer{};
    }
    ActivePass& operator=(ActivePass&& other) noexcept
    {
      if (this != &other) {
        if (cmd) {
          cmd.endRendering();
        }
        cmd = other.cmd;
        other.cmd = vk::CommandBuffer{};
      }
      return *this;
    }
  };

  // Base class for render target states - polymorphic by design
  class RenderTargetState {
  public:
    virtual ~RenderTargetState() = default;
    virtual RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const = 0;
    virtual void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) = 0;
  };

  class SwapchainWithDepthTarget final : public RenderTargetState {
  public:
    RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
    void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  class DeferredGBufferTarget final : public RenderTargetState {
  public:
    RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
    void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  class SwapchainNoDepthTarget final : public RenderTargetState {
  public:
    RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
    void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  struct ClearOps {
    vk::AttachmentLoadOp color = vk::AttachmentLoadOp::eLoad;
    vk::AttachmentLoadOp depth = vk::AttachmentLoadOp::eLoad;
    vk::AttachmentLoadOp stencil = vk::AttachmentLoadOp::eLoad;

    static ClearOps clearAll()
    {
      return { vk::AttachmentLoadOp::eClear, vk::AttachmentLoadOp::eClear, vk::AttachmentLoadOp::eClear };
    }

    static ClearOps loadAll()
    {
      return { vk::AttachmentLoadOp::eLoad, vk::AttachmentLoadOp::eLoad, vk::AttachmentLoadOp::eLoad };
    }

    ClearOps withDepthStencilClear() const
    {
      return { color, vk::AttachmentLoadOp::eClear, vk::AttachmentLoadOp::eClear };
    }
  };

  void endActivePass();

  // Render pass variants - called by target state classes
  void beginSwapchainRenderingInternal(vk::CommandBuffer cmd, uint32_t imageIndex);
  void beginGBufferRenderingInternal(vk::CommandBuffer cmd);
  void beginSwapchainRenderingNoDepthInternal(vk::CommandBuffer cmd, uint32_t imageIndex);

  // Layout transitions (barriers encapsulated here)
  void transitionSwapchainToAttachment(vk::CommandBuffer cmd, uint32_t imageIndex);
  void transitionDepthToAttachment(vk::CommandBuffer cmd);
  void transitionSwapchainToPresent(vk::CommandBuffer cmd, uint32_t imageIndex);
  void transitionGBufferToAttachment(vk::CommandBuffer cmd);
  void transitionGBufferToShaderRead(vk::CommandBuffer cmd);

  VulkanDevice& m_device;
  VulkanRenderTargets& m_targets;

  // Target state - single truth, no pending/active duality
  std::unique_ptr<RenderTargetState> m_target;
  RenderTargetInfo m_activeInfo{};
  std::optional<ActivePass> m_activePass;

  // Clear state
  std::array<float, 4> m_clearColor{0.f, 0.f, 0.f, 1.f};
  float m_clearDepth = 1.0f;
  ClearOps m_clearOps = ClearOps::clearAll();

  // Dynamic state cache
  vk::CullModeFlagBits m_cullMode = vk::CullModeFlagBits::eBack;
  bool m_depthTest = true;
  bool m_depthWrite = true;

    // Swapchain image layout tracking (per swapchain image index)
    std::vector<vk::ImageLayout> m_swapchainLayouts;

  };

} // namespace vulkan
} // namespace graphics
