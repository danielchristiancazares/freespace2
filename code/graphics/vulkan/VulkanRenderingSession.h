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

class VulkanTextureManager;

class VulkanRenderingSession {
public:
  VulkanRenderingSession(VulkanDevice& device,
    VulkanRenderTargets& targets,
    VulkanTextureManager& textures);

    // Frame boundaries - called by VulkanRenderer
    void beginFrame(vk::CommandBuffer cmd, uint32_t imageIndex);
    void endFrame(vk::CommandBuffer cmd, uint32_t imageIndex);

  // Starts dynamic rendering for the *current target* if not already active.
  // Returns the render target contract used for pipeline selection.
  RenderTargetInfo ensureRendering(vk::CommandBuffer cmd, uint32_t imageIndex);
  bool renderingActive() const { return m_activePass.has_value(); }

  // Ends dynamic rendering if currently active. Does not change the selected render target.
  // This is required for transfer operations such as texture updates which are invalid inside rendering.
  void suspendRendering() { endActivePass(); }

  // Boundary-facing state transitions (no "pending", no dual state)
  void requestSwapchainTarget();                  // swapchain + depth
  void requestSwapchainNoDepthTarget();           // swapchain (no depth)
  void requestSceneHdrTarget();                   // scene HDR + depth
  void requestSceneHdrNoDepthTarget();            // scene HDR (no depth)
  void requestPostLdrTarget();                    // post LDR (no depth)
  void requestPostLuminanceTarget();              // post luminance (no depth)
  void requestSmaaEdgesTarget();                  // SMAA edges (no depth)
  void requestSmaaBlendTarget();                  // SMAA blend weights (no depth)
  void requestSmaaOutputTarget();                 // SMAA output (no depth)
  void requestBloomMipTarget(uint32_t pingPongIndex, uint32_t mipLevel); // bloom ping-pong mip (no depth)
  void beginDeferredPass(bool clearNonColorBufs, bool preserveEmissive); // selects gbuffer target
  // Select the deferred G-buffer target without modifying clear/load ops (used by decal pass restore).
  void requestDeferredGBufferTarget();
  void endDeferredGeometry(vk::CommandBuffer cmd);// transitions gbuffer -> shader read, selects swapchain-no-depth
  void requestGBufferEmissiveTarget();            // gbuffer emissive-only (for pre-deferred scene copy)
  void requestGBufferAttachmentTarget(uint32_t gbufferIndex); // single gbuffer attachment (no depth)
  void requestBitmapTarget(int bitmapHandle, int face); // bitmap render target (RTT)

  // Depth attachment selection (OpenGL post-processing parity):
  // - Main depth holds the scene depth
  // - Cockpit depth holds cockpit-only depth (populated between save/restore zbuffer calls)
  void useMainDepthAttachment();
  void useCockpitDepthAttachment();

  // Capture the current swapchain image into the scene-color snapshot for this swapchain image index.
  // No-op if the swapchain was not created with TRANSFER_SRC usage.
  void captureSwapchainColorToSceneCopy(vk::CommandBuffer cmd, uint32_t imageIndex);

  // Transition scene HDR color to shader-read for post-processing.
  void transitionSceneHdrToShaderRead(vk::CommandBuffer cmd);

  // Transition depth attachments to shader-read for post-processing (lightshafts, etc.).
  void transitionMainDepthToShaderRead(vk::CommandBuffer cmd);
  void transitionCockpitDepthToShaderRead(vk::CommandBuffer cmd);

  // Transition offscreen post-processing images to shader-read for sampling in subsequent passes.
  void transitionPostLdrToShaderRead(vk::CommandBuffer cmd);
  void transitionPostLuminanceToShaderRead(vk::CommandBuffer cmd);
  void transitionSmaaEdgesToShaderRead(vk::CommandBuffer cmd);
  void transitionSmaaBlendToShaderRead(vk::CommandBuffer cmd);
  void transitionSmaaOutputToShaderRead(vk::CommandBuffer cmd);
  void transitionBloomToShaderRead(vk::CommandBuffer cmd, uint32_t pingPongIndex);

  // Copy the current scene HDR color into the effect snapshot (used by distortion/effects).
  // Ends any active dynamic rendering scope since transfers are invalid inside rendering.
  void copySceneHdrToEffect(vk::CommandBuffer cmd);

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

  class SceneHdrWithDepthTarget final : public RenderTargetState {
  public:
	RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
	void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  class SceneHdrNoDepthTarget final : public RenderTargetState {
  public:
	RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
	void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  class PostLdrTarget final : public RenderTargetState {
  public:
	RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
	void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  class PostLuminanceTarget final : public RenderTargetState {
  public:
	RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
	void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  class SmaaEdgesTarget final : public RenderTargetState {
  public:
	RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
	void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  class SmaaBlendTarget final : public RenderTargetState {
  public:
	RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
	void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  class SmaaOutputTarget final : public RenderTargetState {
  public:
	RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
	void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  class BloomMipTarget final : public RenderTargetState {
  public:
	BloomMipTarget(uint32_t pingPongIndex, uint32_t mipLevel);
	RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
	void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;

  private:
	uint32_t m_index = 0;
	uint32_t m_mip = 0;
  };

  class DeferredGBufferTarget final : public RenderTargetState {
  public:
    RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
    void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  class GBufferAttachmentTarget final : public RenderTargetState {
  public:
    explicit GBufferAttachmentTarget(uint32_t gbufferIndex);
    RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
    void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;

  private:
    uint32_t m_index = 0;
  };

  class SwapchainNoDepthTarget final : public RenderTargetState {
  public:
    RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
    void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
  };

  class GBufferEmissiveTarget final : public RenderTargetState {
  public:
    RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
    void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t /*imageIndex*/) override;
  };

  class BitmapTarget final : public RenderTargetState {
  public:
    BitmapTarget(int handle, int face, vk::Format format);
    RenderTargetInfo info(const VulkanDevice& device, const VulkanRenderTargets& targets) const override;
    void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t /*imageIndex*/) override;

  private:
    int m_handle = -1;
    int m_face = 0;
    vk::Format m_format = vk::Format::eUndefined;
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
  void beginSceneHdrRenderingInternal(vk::CommandBuffer cmd);
  void beginSceneHdrRenderingNoDepthInternal(vk::CommandBuffer cmd);
  void beginOffscreenColorRenderingInternal(vk::CommandBuffer cmd, vk::Extent2D extent, vk::ImageView colorView);
  void beginGBufferRenderingInternal(vk::CommandBuffer cmd);
  void beginGBufferEmissiveRenderingInternal(vk::CommandBuffer cmd);
  void beginSwapchainRenderingNoDepthInternal(vk::CommandBuffer cmd, uint32_t imageIndex);
  void beginBitmapRenderingInternal(vk::CommandBuffer cmd, int bitmapHandle, int face);

  // Layout transitions (barriers encapsulated here)
  void transitionSwapchainToAttachment(vk::CommandBuffer cmd, uint32_t imageIndex);
  void transitionDepthToAttachment(vk::CommandBuffer cmd);
  void transitionCockpitDepthToAttachment(vk::CommandBuffer cmd);
  void transitionSwapchainToPresent(vk::CommandBuffer cmd, uint32_t imageIndex);
  void transitionGBufferToAttachment(vk::CommandBuffer cmd);
  void transitionGBufferToShaderRead(vk::CommandBuffer cmd);
  void transitionSceneCopyToLayout(vk::CommandBuffer cmd, uint32_t imageIndex, vk::ImageLayout newLayout);
  void transitionSceneHdrToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout);
  void transitionSceneEffectToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout);
  void transitionCockpitDepthToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout);
  void transitionPostLdrToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout);
  void transitionPostLuminanceToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout);
  void transitionSmaaEdgesToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout);
  void transitionSmaaBlendToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout);
  void transitionSmaaOutputToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout);
  void transitionBloomToLayout(vk::CommandBuffer cmd, uint32_t pingPongIndex, vk::ImageLayout newLayout);

  enum class DepthAttachmentKind { Main, Cockpit };
  DepthAttachmentKind m_depthAttachment = DepthAttachmentKind::Main;

  VulkanDevice& m_device;
  VulkanRenderTargets& m_targets;
  VulkanTextureManager& m_textures;

  // Target state - single truth, no pending/active duality
  std::unique_ptr<RenderTargetState> m_target;
  RenderTargetInfo m_activeInfo{};
  std::optional<ActivePass> m_activePass;

  // Clear state
  std::array<float, 4> m_clearColor{0.f, 0.f, 0.f, 1.f};
  float m_clearDepth = 1.0f;
  ClearOps m_clearOps = ClearOps::clearAll();
  std::array<vk::AttachmentLoadOp, VulkanRenderTargets::kGBufferCount> m_gbufferLoadOps{};

  // Dynamic state cache
  vk::CullModeFlagBits m_cullMode = vk::CullModeFlagBits::eBack;
  bool m_depthTest = true;
  bool m_depthWrite = true;

    // Swapchain image layout tracking (per swapchain image index)
    std::vector<vk::ImageLayout> m_swapchainLayouts;

  };

} // namespace vulkan
} // namespace graphics
