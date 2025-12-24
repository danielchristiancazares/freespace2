#pragma once

#include <cstdint>
#include <functional>

#include "VulkanFrame.h"

namespace graphics {
namespace vulkan {

struct SubmitInfo {
  uint32_t imageIndex;
  uint32_t frameIndex;
  uint64_t serial;
  uint64_t timeline;
};

struct RecordingFrame {
  RecordingFrame(VulkanFrame& f, uint32_t img) : frame(f), imageIndex(img) {}

  RecordingFrame(const RecordingFrame&) = delete;
  RecordingFrame& operator=(const RecordingFrame&) = delete;
  RecordingFrame(RecordingFrame&&) = default;
  RecordingFrame& operator=(RecordingFrame&&) = default;

  VulkanFrame& ref() const { return frame.get(); }

  private:
  std::reference_wrapper<VulkanFrame> frame;
  uint32_t imageIndex;

  vk::CommandBuffer cmd() const { return frame.get().commandBuffer(); }

  friend class VulkanRenderer;
};

struct InFlightFrame {
  std::reference_wrapper<VulkanFrame> frame;
  SubmitInfo submit;

  InFlightFrame(VulkanFrame& f, SubmitInfo s) : frame(f), submit(s) {}

  InFlightFrame(const InFlightFrame&) = delete;
  InFlightFrame& operator=(const InFlightFrame&) = delete;
  InFlightFrame(InFlightFrame&&) = default;
  InFlightFrame& operator=(InFlightFrame&&) = default;

  VulkanFrame& ref() const { return frame.get(); }
};

} // namespace vulkan
} // namespace graphics



