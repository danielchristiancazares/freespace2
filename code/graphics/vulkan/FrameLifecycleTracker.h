#pragma once

#include <cstdint>

namespace graphics {
namespace vulkan {

// Lightweight tracker for command recording state across frames.
class FrameLifecycleTracker {
  public:
	void begin(uint32_t frame_index);
	void end();

	bool isRecording() const { return m_isRecording; }
	uint32_t currentFrameIndex() const { return m_frameIndex; }
	bool warnOnceIfNotRecording(); // returns true if this was the first warning for the frame

  private:
	bool m_isRecording = false;
	uint32_t m_frameIndex = 0;
	bool m_warnedNoRecordingThisFrame = false;
};

} // namespace vulkan
} // namespace graphics
