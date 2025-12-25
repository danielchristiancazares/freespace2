#include "VulkanShaderManager.h"

#include "VulkanRenderer.h"
#include "globalincs/pstypes.h"
#include "def_files/def_files.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <chrono>
#include <atomic>
#include <string>

namespace fs = std::filesystem;

namespace graphics {
namespace vulkan {

VulkanShaderManager::VulkanShaderManager(vk::Device device, const SCP_string& shaderRoot)
  : m_device(device), m_shaderRoot(shaderRoot)
{
}

ShaderModules VulkanShaderManager::getModules(shader_type type, uint32_t variantFlags)
{
  Key key{type, variantFlags};

  auto loadIfMissing = [&](std::unordered_map<Key, vk::UniqueShaderModule, KeyHasher>& cache,
			   const SCP_string& filename) -> vk::ShaderModule {
	auto it = cache.find(key);
	if (it != cache.end()) {
	  return it->second.get();
	}

	auto module = loadModule(filename);
	auto handle = module.get();
	cache.emplace(key, std::move(module));
	return handle;
  };

  switch (type) {
  case shader_type::SDR_TYPE_MODEL: {
	// Model path uses a unified shader pair; ignore variant flags for module lookup/cache.
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "model.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "model.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_DEFAULT_MATERIAL: {
	const auto vertPath = fs::path(m_shaderRoot) / "default-material.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "default-material.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_BATCHED_BITMAP: {
	const auto vertPath = fs::path(m_shaderRoot) / "batched-bitmap.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "batched-bitmap.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_INTERFACE: {
	const auto vertPath = fs::path(m_shaderRoot) / "interface.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "interface.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
	case shader_type::SDR_TYPE_NANOVG: {
	  // NanoVG path uses a unified shader pair; ignore variant flags for module lookup/cache.
	  key.flags = 0;
	  const auto vertPath = fs::path(m_shaderRoot) / "nanovg.vert.spv";
	  const auto fragPath = fs::path(m_shaderRoot) / "nanovg.frag.spv";
	  return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
	}
	case shader_type::SDR_TYPE_ROCKET_UI: {
	  // Rocket UI path uses a unified shader pair; ignore variant flags for module lookup/cache.
	  key.flags = 0;
	  const auto vertPath = fs::path(m_shaderRoot) / "rocketui.vert.spv";
	  const auto fragPath = fs::path(m_shaderRoot) / "rocketui.frag.spv";
	  return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
	}
  case shader_type::SDR_TYPE_PASSTHROUGH_RENDER: {
	const auto vertPath = fs::path(m_shaderRoot) / "vulkan.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "vulkan.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_COPY: {
	const auto vertPath = fs::path(m_shaderRoot) / "copy.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "copy.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_POST_PROCESS_BRIGHTPASS: {
	// Bloom bright-pass: downsample + high-pass into half-res RGBA16F.
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "post_uv.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "brightpass.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_POST_PROCESS_BLUR: {
	// Bloom blur: horizontal/vertical variants selected by SDR_FLAG_BLUR_*.
	const auto vertPath = fs::path(m_shaderRoot) / "post_uv.vert.spv";
	const auto fragPath =
	  (variantFlags & SDR_FLAG_BLUR_HORIZONTAL)
		? (fs::path(m_shaderRoot) / "blur_h.frag.spv")
		: (fs::path(m_shaderRoot) / "blur_v.frag.spv");
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_POST_PROCESS_BLOOM_COMP: {
	// Bloom composite: sample blurred mip chain and add into HDR scene color.
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "post_uv.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "bloom_comp.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_POST_PROCESS_SMAA_EDGE: {
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "smaa_edge.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "smaa_edge.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT: {
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "smaa_blend.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "smaa_blend.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING: {
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "smaa_neighborhood.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "smaa_neighborhood.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_POST_PROCESS_FXAA_PREPASS: {
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "post_uv.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "fxaa_prepass.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_POST_PROCESS_FXAA: {
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "post_uv.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "fxaa.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_POST_PROCESS_LIGHTSHAFTS: {
	// Lightshafts: additive fullscreen pass into LDR.
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "post_uv.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "lightshafts.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_POST_PROCESS_MAIN: {
	// Main post-processing shader (color grading / film grain etc) applied on the LDR image.
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "post_uv.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "post_effects.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_POST_PROCESS_TONEMAPPING: {
	// Vulkan tonemapping pass: always outputs linear (swapchain is sRGB).
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "tonemapping.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "tonemapping.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_DEFERRED_LIGHTING: {
	const auto vertPath = fs::path(m_shaderRoot) / "deferred.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "deferred.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_FLAT_COLOR: {
	const auto vertPath = fs::path(m_shaderRoot) / "flat-color.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "flat-color.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  case shader_type::SDR_TYPE_SHIELD_DECAL: {
	// Shield impact: unified module pair; ignore variant flags for module lookup/cache.
	key.flags = 0;
	const auto vertPath = fs::path(m_shaderRoot) / "shield-impact.vert.spv";
	const auto fragPath = fs::path(m_shaderRoot) / "shield-impact.frag.spv";
	return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
  }
  default:
	// Any shader type not explicitly mapped is unsupported on Vulkan; fail fast
	throw std::runtime_error("Unsupported shader_type for Vulkan: type=" + std::to_string(static_cast<int>(type)) +
							 " flags=0x" + std::to_string(variantFlags));
  }
}

ShaderModules VulkanShaderManager::getModulesByFilenames(const SCP_string& vertFilename, const SCP_string& fragFilename)
{
  return { loadModuleByFilename(vertFilename), loadModuleByFilename(fragFilename) };
}

vk::ShaderModule VulkanShaderManager::loadModuleByFilename(const SCP_string& filename)
{
  auto it = m_filenameModules.find(filename);
  if (it != m_filenameModules.end()) {
	return it->second.get();
  }

  const auto fullPath = (fs::path(m_shaderRoot) / filename).string();
  auto module = loadModule(fullPath);
  auto handle = module.get();
  m_filenameModules.emplace(filename, std::move(module));
  return handle;
}

vk::UniqueShaderModule VulkanShaderManager::loadModule(const SCP_string& path)
{
  // Try embedded file first (path stripped to filename)
  const auto filename = fs::path(path).filename().string();
  const auto embedded = defaults_get_all();
  bool foundEmbedded = false;
  for (const auto& df : embedded) {
	// Embedded files are stored with a prefix (e.g. "data/effects/...") so compare by basename.
	const auto embeddedName = fs::path(df.filename).filename().string();
	if (!stricmp(embeddedName.c_str(), filename.c_str())) {
	  foundEmbedded = true;

	  // Ensure alignment by copying to a uint32_t vector if needed
	  std::vector<uint32_t> code((df.size + 3) / 4);
	  std::memcpy(code.data(), df.data, df.size);

	  vk::ShaderModuleCreateInfo moduleInfo;
	  moduleInfo.codeSize = df.size;
	  moduleInfo.pCode = code.data();

	  auto module = m_device.createShaderModuleUnique(moduleInfo);
	  return module;
	}
  }

  // Fallback to filesystem

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
	throw std::runtime_error("Failed to open shader module " + path);
  }

  auto fileSize = static_cast<size_t>(file.tellg());
  // Use uint32_t vector to ensure 4-byte alignment required by Vulkan spec for pCode
  std::vector<uint32_t> buffer((fileSize + 3) / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));

  vk::ShaderModuleCreateInfo moduleInfo;
  moduleInfo.codeSize = fileSize;
  moduleInfo.pCode = buffer.data();

  auto module = m_device.createShaderModuleUnique(moduleInfo);
  return module;
}

} // namespace vulkan
} // namespace graphics
