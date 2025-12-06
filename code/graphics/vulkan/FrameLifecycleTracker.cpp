#include "FrameLifecycleTracker.h"

namespace graphics {
namespace vulkan {

void FrameLifecycleTracker::begin(uint32_t frame_index)
{
	m_isRecording = true;
	m_frameIndex = frame_index;
	m_warnedNoRecordingThisFrame = false;
}

void FrameLifecycleTracker::end()
{
	m_isRecording = false;
}

bool FrameLifecycleTracker::warnOnceIfNotRecording()
{
	if (m_isRecording || m_warnedNoRecordingThisFrame) {
		return false;
	}
	m_warnedNoRecordingThisFrame = true;
	return true;
}

} // namespace vulkan
} // namespace graphics
