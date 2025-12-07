#include "VulkanShaderManager.h"

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

		auto module = loadModule(filename);
		auto handle = module.get();
		cache.emplace(key, std::move(module));
		return handle;
	};

	switch (type) {
	case shader_type::SDR_TYPE_DEFAULT_MATERIAL: {
		const auto vertPath = fs::path(m_shaderRoot) / "default-material.vert.spv";
		const auto fragPath = fs::path(m_shaderRoot) / "default-material.frag.spv";
		return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
		}
	case shader_type::SDR_TYPE_PASSTHROUGH_RENDER: {
		const auto vertPath = fs::path(m_shaderRoot) / "vulkan.vert.spv";
		const auto fragPath = fs::path(m_shaderRoot) / "vulkan.frag.spv";
		return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
	}
	default:
		{
			const auto vertPath = fs::path(m_shaderRoot) / "vulkan.vert.spv";
			const auto fragPath = fs::path(m_shaderRoot) / "vulkan.frag.spv";
			return {loadIfMissing(m_vertexModules, vertPath.string()), loadIfMissing(m_fragmentModules, fragPath.string())};
		}
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

			return m_device.createShaderModuleUnique(moduleInfo);
		}
	}

	// Fallback to filesystem
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		throw std::runtime_error("Failed to open shader module " + path);
	}

	auto fileSize = static_cast<size_t>(file.tellg());
	std::vector<char> buffer(fileSize);
	file.seekg(0);
	file.read(buffer.data(), fileSize);

	vk::ShaderModuleCreateInfo moduleInfo;
	moduleInfo.codeSize = buffer.size();
	moduleInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

	return m_device.createShaderModuleUnique(moduleInfo);
}

} // namespace vulkan
} // namespace graphics
