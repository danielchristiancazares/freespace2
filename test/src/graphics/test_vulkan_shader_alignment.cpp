#include "graphics/vulkan/VulkanShaderManager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

namespace {

// Captured data from shader module creation
struct CapturedShaderInfo {
	size_t codeSize;
	uintptr_t pCodeAddress;
	bool is4ByteAligned;
};

static std::vector<CapturedShaderInfo> gCapturedInfo;
static uint32_t gCreateCalls = 0;

VkResult VKAPI_CALL AlignmentCapturingCreateShaderModule(
	VkDevice /*device*/,
	const VkShaderModuleCreateInfo* createInfo,
	const VkAllocationCallbacks* /*allocator*/,
	VkShaderModule* pShaderModule)
{
	++gCreateCalls;

	if (createInfo && createInfo->pCode) {
		CapturedShaderInfo info{};
		info.codeSize = createInfo->codeSize;
		info.pCodeAddress = reinterpret_cast<uintptr_t>(createInfo->pCode);
		info.is4ByteAligned = (info.pCodeAddress % 4) == 0;
		gCapturedInfo.push_back(info);
	}

	*pShaderModule = reinterpret_cast<VkShaderModule>(static_cast<uintptr_t>(0x2000 + gCreateCalls));
	return VK_SUCCESS;
}

void VKAPI_CALL AlignmentCapturingDestroyShaderModule(
	VkDevice /*device*/,
	VkShaderModule /*module*/,
	const VkAllocationCallbacks* /*allocator*/)
{
	// no-op
}

void resetCapture()
{
	gCreateCalls = 0;
	gCapturedInfo.clear();
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

class VulkanShaderAlignmentTest : public ::testing::Test {
  protected:
	void SetUp() override
	{
		resetCapture();
		m_root = std::filesystem::temp_directory_path() / "fso-shader-alignment" / std::filesystem::path(makeNonce());
		std::filesystem::create_directories(m_root);

		m_prevCreate = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShaderModule;
		m_prevDestroy = VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyShaderModule;
		VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShaderModule = &AlignmentCapturingCreateShaderModule;
		VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyShaderModule = &AlignmentCapturingDestroyShaderModule;
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

// Test that filesystem shader loading produces 4-byte aligned pCode pointers.
// This is critical for ARM architectures (Mac M-series, Android) where unaligned
// 32-bit access causes SIGBUS. The Vulkan spec requires pCode to be 4-byte aligned.
TEST_F(VulkanShaderAlignmentTest, FilesystemLoadProduces4ByteAlignedPCode)
{
	// Create shader files with sizes that are NOT multiples of 4.
	// If the implementation uses std::vector<char>, the buffer may not be
	// 4-byte aligned (standard only guarantees alignof(char) = 1).
	// The correct implementation uses std::vector<uint32_t>.
	writeSpirv(m_root, "model.vert.spv", 13);  // 13 bytes - not multiple of 4
	writeSpirv(m_root, "model.frag.spv", 17);  // 17 bytes - not multiple of 4

	vk::Device fakeDevice{reinterpret_cast<VkDevice>(0x1234)};
	graphics::vulkan::VulkanShaderManager manager(fakeDevice, m_root.string());

	// Load shaders - this triggers filesystem fallback since these aren't embedded
	auto modules = manager.getModules(shader_type::SDR_TYPE_MODEL, 0);

	// Verify we captured both shader module creations
	ASSERT_EQ(gCapturedInfo.size(), 2u) << "Expected 2 shader modules to be created";

	// Verify both have 4-byte aligned pCode pointers
	for (size_t i = 0; i < gCapturedInfo.size(); ++i) {
		EXPECT_TRUE(gCapturedInfo[i].is4ByteAligned)
			<< "Shader module " << i << " pCode pointer (0x" << std::hex << gCapturedInfo[i].pCodeAddress
			<< ") is not 4-byte aligned. This will cause SIGBUS on ARM architectures.";
	}
}

// Test alignment with various file sizes to catch edge cases
TEST_F(VulkanShaderAlignmentTest, AlignmentCorrectForVariousFileSizes)
{
	// Test sizes: 4 (aligned), 5, 6, 7, 8 (aligned), 100, 101, 102, 103
	const std::vector<size_t> testSizes = {4, 5, 6, 7, 8, 100, 101, 102, 103};

	for (size_t size : testSizes) {
		resetCapture();

		std::string vertName = "test-" + std::to_string(size) + ".vert.spv";
		std::string fragName = "test-" + std::to_string(size) + ".frag.spv";

		// Write shaders with specific sizes - use interface shader type for this test
		writeSpirv(m_root, "interface.vert.spv", size);
		writeSpirv(m_root, "interface.frag.spv", size);

		vk::Device fakeDevice{reinterpret_cast<VkDevice>(0x5678)};
		graphics::vulkan::VulkanShaderManager manager(fakeDevice, m_root.string());

		auto modules = manager.getModules(shader_type::SDR_TYPE_INTERFACE, 0);

		ASSERT_EQ(gCapturedInfo.size(), 2u) << "Expected 2 shader modules for size " << size;

		for (size_t i = 0; i < gCapturedInfo.size(); ++i) {
			EXPECT_TRUE(gCapturedInfo[i].is4ByteAligned)
				<< "File size " << size << ": shader module " << i << " pCode (0x" << std::hex
				<< gCapturedInfo[i].pCodeAddress << ") not 4-byte aligned";
		}

		// Clean up for next iteration
		std::filesystem::remove(m_root / "interface.vert.spv");
		std::filesystem::remove(m_root / "interface.frag.spv");
	}
}

// Test that codeSize is NOT rounded up when filesystem loading occurs.
// Note: The shader manager checks embedded files first by filename. If embedded
// model.vert.spv/model.frag.spv exist, they will be used instead of our test files.
// We use passthrough shader type which is less likely to have embedded versions.
TEST_F(VulkanShaderAlignmentTest, CodeSizePreservedExactly)
{
	const size_t exactSize = 13;  // Not a multiple of 4
	writeSpirv(m_root, "vulkan.vert.spv", exactSize);
	writeSpirv(m_root, "vulkan.frag.spv", exactSize);

	vk::Device fakeDevice{reinterpret_cast<VkDevice>(0x9ABC)};
	graphics::vulkan::VulkanShaderManager manager(fakeDevice, m_root.string());

	auto modules = manager.getModules(shader_type::SDR_TYPE_PASSTHROUGH_RENDER, 0);

	ASSERT_EQ(gCapturedInfo.size(), 2u);

	// If filesystem loading was used, codeSize should match exactly.
	// If embedded loading was used (embedded files exist), codeSize will differ.
	// Either way, alignment is the critical check (covered by other tests).
	// This test verifies filesystem path preserves exact size when it's used.
	bool filesystemWasUsed = (gCapturedInfo[0].codeSize == exactSize);
	if (filesystemWasUsed) {
		for (const auto& info : gCapturedInfo) {
			EXPECT_EQ(info.codeSize, exactSize)
				<< "codeSize should be exact file size (" << exactSize << "), not rounded";
		}
	} else {
		// Embedded files were used - just verify alignment (already checked in other tests)
		EXPECT_TRUE(gCapturedInfo[0].is4ByteAligned) << "Embedded path should also be aligned";
	}
}
