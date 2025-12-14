#pragma once

#include "graphics/2d.h"

#include "globalincs/pstypes.h"

#include <vector>

namespace graphics {
namespace vulkan {

enum class PipelineLayoutKind {
	Standard, // per-draw push descriptors + global set
	Model,    // model bindless set + push constants
	Deferred  // deferred lighting push descriptors + global (G-buffer) set
};

enum class VertexInputMode {
	VertexAttributes, // traditional vertex attributes from vertex_layout
	VertexPulling     // no vertex attributes; fetch from buffers in shader
};

struct ShaderLayoutSpec {
	shader_type type;
	const char* name;
	PipelineLayoutKind pipelineLayout;
	VertexInputMode vertexInput;
};

// Returns the explicit layout contract for a shader_type (asserts on invalid type)
const ShaderLayoutSpec& getShaderLayoutSpec(shader_type type);

// Returns the full set of layout contracts, indexed by shader_type value
const SCP_vector<ShaderLayoutSpec>& getShaderLayoutSpecs();

// Convenience helpers
bool usesVertexPulling(shader_type type);
PipelineLayoutKind pipelineLayoutForShader(shader_type type);

} // namespace vulkan
} // namespace graphics














