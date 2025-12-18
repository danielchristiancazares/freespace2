#pragma once

#include "osapi/osapi.h"

namespace graphics {
namespace metal {

void initialize_function_pointers();
bool initialize(std::unique_ptr<os::GraphicsOperations>&& graphicsOps);
void cleanup();

} // namespace metal
} // namespace graphics
