#include "VulkanDebug.h"

#include "cmdline/cmdline.h"
#include "osapi/outwnd.h"
#include <cstdarg>
#include <cstdio>

namespace graphics {
namespace vulkan {

void vkprintf(const char* format, ...)
{
	if (!(FSO_DEBUG || Cmdline_graphics_debug_output)) {
		return;
	}

	va_list args;
	va_start(args, format);
	char buffer[2048];
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	nprintf(("Vulkan", "%s", buffer));
	// Flush all streams to ensure the log entry is persisted even if we crash immediately after.
	fflush(nullptr);
}

} // namespace vulkan
} // namespace graphics

