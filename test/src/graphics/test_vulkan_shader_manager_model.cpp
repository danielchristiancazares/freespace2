#include "graphics/vulkan/VulkanShaderManager.h"
#include "graphics/vulkan/VulkanPipelineManager.h"
#include "graphics/vulkan/VulkanModelValidation.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

namespace {

// Stubbed Vulkan entry points to avoid real driver interaction
static uint32_t gCreateCalls = 0;
static std::vector<size_t> gCodeSizes;

VkResult VKAPI_CALL StubCreateShaderModule(
	VkDevice /*device*/,
	const VkShaderModuleCreateInfo* createInfo,
	const VkAllocationCallbacks* /*allocator*/,
	VkShaderModule* pShaderModule)
{
	++gCreateCalls;
	gCodeSizes.push_back(createInfo ? static_cast<size_t>(createInfo->codeSize) : 0u);
	*pShaderModule = reinterpret_cast<VkShaderModule>(static_cast<uintptr_t>(0x1000 + gCreateCalls));
	return VK_SUCCESS;
}

void VKAPI_CALL StubDestroyShaderModule(
	VkDevice /*device*/,
	VkShaderModule /*module*/,
	const VkAllocationCallbacks* /*allocator*/)
{
	// no-op
}

void resetStub()
{
	gCreateCalls = 0;
	gCodeSizes.clear();
}

std::string makeNonce()
{
	std::random_device rd;
	std::mt19937_64 gen(rd());
	std::uniform_int_distribution<uint64_t> dist;
	std::stringstream ss;
	ss << std::hex << dist(gen);
	return ss.str();
}

std::vector<uint8_t> makeSpirvPayload(size_t bytes)
{
	std::vector<uint8_t> data(bytes, 0);
	if (bytes >= 4) {
		// SPIR-V magic number 0x07230203
		data[0] = 0x03;
		data[1] = 0x02;
		data[2] = 0x23;
		data[3] = 0x07;
	}
	return data;
}

void writeSpirv(const std::filesystem::path& root, const std::string& name, size_t bytes)
{
	auto path = root / name;
	std::filesystem::create_directories(path.parent_path());
	auto payload = makeSpirvPayload(bytes);
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
}

class VulkanShaderManagerModelTest : public ::testing::Test {
  protected:
	void SetUp() override
	{
		resetStub();
		m_root = std::filesystem::temp_directory_path() / "fso-model-shader-manager" /
			std::filesystem::path(makeNonce());
		std::filesystem::create_directories(m_root);

		m_prevCreate = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShaderModule;
		m_prevDestroy = VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyShaderModule;
		VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShaderModule = &StubCreateShaderModule;
		VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyShaderModule = &StubDestroyShaderModule;
	}

	void TearDown() override
	{
		VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShaderModule = m_prevCreate;
		VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyShaderModule = m_prevDestroy;

		std::error_code ec;
		std::filesystem::remove_all(m_root, ec);
	}

	std::filesystem::path m_root;
	PFN_vkCreateShaderModule m_prevCreate{nullptr};
	PFN_vkDestroyShaderModule m_prevDestroy{nullptr};
};

} // namespace

TEST_F(VulkanShaderManagerModelTest, Scenario_ModelUsesUnifiedModules)
{
	// Provide shader pairs for both model and default-material paths.
	// The shader manager checks embedded files first by filename, so we provide
	// filesystem fallbacks. The test verifies that SDR_TYPE_MODEL routes to
	// different shaders than SDR_TYPE_DEFAULT_MATERIAL (the contract), regardless
	// of whether they come from embedded files or the filesystem.
	writeSpirv(m_root, "model.vert.spv", 8);
	writeSpirv(m_root, "model.frag.spv", 12);
	writeSpirv(m_root, "default-material.vert.spv", 100);
	writeSpirv(m_root, "default-material.frag.spv", 104);

	vk::Device fakeDevice{reinterpret_cast<VkDevice>(0x1234)};
	graphics::vulkan::VulkanShaderManager manager(fakeDevice, m_root.string());

	auto modelModules = manager.getModules(shader_type::SDR_TYPE_MODEL, 0);
	auto defaultModules = manager.getModules(shader_type::SDR_TYPE_DEFAULT_MATERIAL, 0);

	// SDR_TYPE_MODEL must use distinct shader modules from SDR_TYPE_DEFAULT_MATERIAL.
	// If model falls through to the default case (bug), these would be equal.
	EXPECT_NE(modelModules.vert, defaultModules.vert)
		<< "Model vertex shader should differ from default-material vertex shader";
	EXPECT_NE(modelModules.frag, defaultModules.frag)
		<< "Model fragment shader should differ from default-material fragment shader";
}

TEST_F(VulkanShaderManagerModelTest, Scenario_ModelIgnoresVariantFlagsForModules)
{
	writeSpirv(m_root, "model.vert.spv", 16);
	writeSpirv(m_root, "model.frag.spv", 24);

	vk::Device fakeDevice{reinterpret_cast<VkDevice>(0x1234)};
	graphics::vulkan::VulkanShaderManager manager(fakeDevice, m_root.string());

	auto first = manager.getModules(shader_type::SDR_TYPE_MODEL, 0u);
	auto second = manager.getModules(shader_type::SDR_TYPE_MODEL, 0xFFu); // arbitrary flags

	// Unified shader set should return the same module handles regardless of flags
	EXPECT_EQ(first.vert, second.vert);
	EXPECT_EQ(first.frag, second.frag);
}

TEST_F(VulkanShaderManagerModelTest, Scenario_ModelPipelineKeyIgnoresLayoutHash)
{
	graphics::vulkan::PipelineKey a{};
	a.type = shader_type::SDR_TYPE_MODEL;
	a.variant_flags = 0;
	a.color_format = static_cast<VkFormat>(vk::Format::eB8G8R8A8Unorm);
	a.depth_format = static_cast<VkFormat>(vk::Format::eD32Sfloat);
	a.sample_count = VK_SAMPLE_COUNT_1_BIT;
	a.color_attachment_count = 1;
	a.blend_mode = ALPHA_BLEND_NONE;
	a.layout_hash = 0xAAAA;

	auto b = a;
	b.layout_hash = 0xBBBB; // different vertex layout hash

	// For model vertex-pulling, layout hash should be ignored; keys should compare equal
	EXPECT_TRUE(a == b);
}

TEST_F(VulkanShaderManagerModelTest, Scenario_ModelRequiresDescriptorIndexingFeatures)
{
	// Missing critical descriptor indexing features should return false
	vk::PhysicalDeviceDescriptorIndexingFeatures feats{};
	feats.shaderSampledImageArrayNonUniformIndexing = VK_FALSE; // required
	feats.runtimeDescriptorArray = VK_FALSE; // required
	feats.descriptorBindingVariableDescriptorCount = VK_FALSE; // required
	feats.descriptorBindingSampledImageUpdateAfterBind = VK_FALSE; // required

	EXPECT_FALSE(graphics::vulkan::ValidateModelDescriptorIndexingSupport(feats));
}

TEST_F(VulkanShaderManagerModelTest, Scenario_ModelPushConstantSizeGuard)
{
	// Required push-constant footprint exceeds device limit; should throw
	const uint32_t required = 512;
	const uint32_t deviceLimit = 256;
	EXPECT_THROW(graphics::vulkan::EnsureModelPushConstantBudget(required, deviceLimit), std::runtime_error);
}

// Integration tests: verify the Vulkan12Features overload extracts and validates correctly.
// This tests the WIRING - the path from device features struct to validation result.
// If someone breaks the field extraction, these tests fail.

TEST(DeviceFeatureValidation, AcceptsVulkan12FeaturesWithAllRequired)
{
	vk::PhysicalDeviceVulkan12Features features12{};
	features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	features12.runtimeDescriptorArray = VK_TRUE;
	features12.descriptorBindingPartiallyBound = VK_TRUE;

	// This overload is called by isDeviceUnsuitable during device selection
	EXPECT_TRUE(graphics::vulkan::ValidateModelDescriptorIndexingSupport(features12));
}

TEST(DeviceFeatureValidation, RejectsVulkan12FeaturesWhenAnyMissing)
{
	// Baseline: all required features present
	vk::PhysicalDeviceVulkan12Features baseline{};
	baseline.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	baseline.runtimeDescriptorArray = VK_TRUE;
	baseline.descriptorBindingPartiallyBound = VK_TRUE;
	ASSERT_TRUE(graphics::vulkan::ValidateModelDescriptorIndexingSupport(baseline));

	// Test each required feature individually
	{
		auto f = baseline;
		f.shaderSampledImageArrayNonUniformIndexing = VK_FALSE;
		EXPECT_FALSE(graphics::vulkan::ValidateModelDescriptorIndexingSupport(f))
			<< "Should reject when shaderSampledImageArrayNonUniformIndexing is missing";
	}
	{
		auto f = baseline;
		f.runtimeDescriptorArray = VK_FALSE;
		EXPECT_FALSE(graphics::vulkan::ValidateModelDescriptorIndexingSupport(f))
			<< "Should reject when runtimeDescriptorArray is missing";
	}
	{
		auto f = baseline;
		f.descriptorBindingPartiallyBound = VK_FALSE;
		EXPECT_FALSE(graphics::vulkan::ValidateModelDescriptorIndexingSupport(f))
			<< "Should reject when descriptorBindingPartiallyBound is missing";
	}
}
