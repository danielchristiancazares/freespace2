#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

struct StageAccess {
  vk::PipelineStageFlags2 stageMask{};
  vk::AccessFlags2 accessMask{};
};

inline StageAccess stageAccessForLayout(vk::ImageLayout layout) {
  StageAccess out{};
  switch (layout) {
  case vk::ImageLayout::eUndefined:
    out.stageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
    out.accessMask = {};
    break;
  case vk::ImageLayout::eColorAttachmentOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    out.accessMask = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    break;
  case vk::ImageLayout::eDepthAttachmentOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    out.accessMask =
        vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    break;
  case vk::ImageLayout::eDepthStencilAttachmentOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    out.accessMask =
        vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    break;
  case vk::ImageLayout::eShaderReadOnlyOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    out.accessMask = vk::AccessFlagBits2::eShaderRead;
    break;
  case vk::ImageLayout::eTransferSrcOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eTransfer;
    out.accessMask = vk::AccessFlagBits2::eTransferRead;
    break;
  case vk::ImageLayout::eTransferDstOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eTransfer;
    out.accessMask = vk::AccessFlagBits2::eTransferWrite;
    break;
  case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    out.accessMask = vk::AccessFlagBits2::eShaderRead;
    break;
  case vk::ImageLayout::ePresentSrcKHR:
    // Present is external to the pipeline. For sync2 barriers that transition to/from present,
    // destination stage/access should typically be NONE/0.
    out.stageMask = {};
    out.accessMask = {};
    break;
  default:
    out.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
    out.accessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    break;
  }
  return out;
}

inline void submitImageBarriers(vk::CommandBuffer cmd, const vk::ImageMemoryBarrier2 *barriers, uint32_t count) {
  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = count;
  dep.pImageMemoryBarriers = barriers;
  cmd.pipelineBarrier2(dep);
}

inline void submitImageBarrier(vk::CommandBuffer cmd, const vk::ImageMemoryBarrier2 &barrier) {
  submitImageBarriers(cmd, &barrier, 1);
}

inline vk::ImageSubresourceRange makeSubresourceRange(vk::ImageAspectFlags aspectMask, uint32_t baseMipLevel,
                                                      uint32_t levelCount, uint32_t baseArrayLayer,
                                                      uint32_t layerCount) {
  vk::ImageSubresourceRange range{};
  range.aspectMask = aspectMask;
  range.baseMipLevel = baseMipLevel;
  range.levelCount = levelCount;
  range.baseArrayLayer = baseArrayLayer;
  range.layerCount = layerCount;
  return range;
}

inline vk::ImageMemoryBarrier2 makeImageLayoutBarrier(vk::Image image, vk::ImageLayout oldLayout,
                                                      vk::ImageLayout newLayout, vk::ImageSubresourceRange range) {
  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = image;
  barrier.subresourceRange = range;
  return barrier;
}

inline vk::ImageMemoryBarrier2 makeImageLayoutBarrier(vk::Image image, vk::ImageLayout oldLayout,
                                                      vk::ImageLayout newLayout, vk::ImageAspectFlags aspectMask,
                                                      uint32_t baseMipLevel, uint32_t levelCount,
                                                      uint32_t baseArrayLayer, uint32_t layerCount) {
  return makeImageLayoutBarrier(image, oldLayout, newLayout,
                                makeSubresourceRange(aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount));
}

inline vk::ImageMemoryBarrier2 makeImageLayoutBarrier(vk::Image image, vk::ImageLayout oldLayout,
                                                      vk::ImageLayout newLayout, vk::ImageAspectFlags aspectMask,
                                                      uint32_t levelCount, uint32_t layerCount) {
  return makeImageLayoutBarrier(image, oldLayout, newLayout, aspectMask, 0, levelCount, 0, layerCount);
}

inline void transitionImageLayout(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout oldLayout,
                                  vk::ImageLayout newLayout, vk::ImageSubresourceRange range) {
  if (oldLayout == newLayout) {
    return;
  }

  const auto barrier = makeImageLayoutBarrier(image, oldLayout, newLayout, range);
  submitImageBarrier(cmd, barrier);
}

inline void transitionImageLayout(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout oldLayout,
                                  vk::ImageLayout newLayout, vk::ImageAspectFlags aspectMask, uint32_t baseMipLevel,
                                  uint32_t levelCount, uint32_t baseArrayLayer, uint32_t layerCount) {
  transitionImageLayout(cmd, image, oldLayout, newLayout,
                        makeSubresourceRange(aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount));
}

inline void transitionImageLayout(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout oldLayout,
                                  vk::ImageLayout newLayout, vk::ImageAspectFlags aspectMask, uint32_t levelCount,
                                  uint32_t layerCount) {
  transitionImageLayout(cmd, image, oldLayout, newLayout, aspectMask, 0, levelCount, 0, layerCount);
}

} // namespace vulkan
} // namespace graphics
