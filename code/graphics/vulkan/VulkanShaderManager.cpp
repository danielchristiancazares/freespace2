#include "VulkanShaderManager.h"

#include "VulkanRenderer.h"
#include "globalincs/pstypes.h"
#include "def_files/def_files.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <cstring>

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

		vkprintf("Loading shader module: %s (type=%d, flags=0x%x)\n", 
			filename.c_str(), static_cast<int>(type), variantFlags);
		auto module = loadModule(filename);
		auto handle = module.get();
		cache.emplace(key, std::move(module));
		vkprintf("Shader module loaded successfully: %s -> %p\n", filename.c_str(), static_cast<const void*>(handle));
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
	case shader_type::SDR_TYPE_PASSTHROUGH_RENDER: {
		const auto vertPath = fs::path(m_shaderRoot) / "vulkan.vert.spv";
		const auto fragPath = fs::path(m_shaderRoot) / "vulkan.frag.spv";
		return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
	}
	default:
		// Any shader type not explicitly mapped is unsupported on Vulkan; fail fast
		throw std::runtime_error("Unsupported shader_type for Vulkan: type=" + std::to_string(static_cast<int>(type)) +
		                         " flags=0x" + std::to_string(variantFlags));
	}
}

vk::UniqueShaderModule VulkanShaderManager::loadModule(const SCP_string& path)
{
	// Try embedded file first (path stripped to filename)
	const auto filename = fs::path(path).filename().string();
	const auto embedded = defaults_get_all();
	for (const auto& df : embedded) {
		if (!stricmp(df.filename, filename.c_str())) {
			// Ensure alignment by copying to a uint32_t vector if needed
			std::vector<uint32_t> code((df.size + 3) / 4);
			std::memcpy(code.data(), df.data, df.size);

			vk::ShaderModuleCreateInfo moduleInfo;
			moduleInfo.codeSize = df.size;
			moduleInfo.pCode = code.data();

			auto module = m_device.createShaderModuleUnique(moduleInfo);
			vkprintf("Loaded shader module from embedded: %s (size=%zu)\n", filename.c_str(), df.size);
			return module;
		}
	}

	// Fallback to filesystem
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		vkprintf("ERROR - Failed to open shader module: %s\n", path.c_str());
		throw std::runtime_error("Failed to open shader module " + path);
	}

	auto fileSize = static_cast<size_t>(file.tellg());
	std::vector<char> buffer(fileSize);
	file.seekg(0);
	file.read(buffer.data(), fileSize);

	vk::ShaderModuleCreateInfo moduleInfo;
	moduleInfo.codeSize = buffer.size();
	moduleInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

	auto module = m_device.createShaderModuleUnique(moduleInfo);
	vkprintf("Loaded shader module from filesystem: %s (size=%zu)\n", path.c_str(), fileSize);
	return module;
}

} // namespace vulkan
} // namespace graphics
