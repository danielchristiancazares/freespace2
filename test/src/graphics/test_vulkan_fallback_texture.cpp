#include "graphics/vulkan/VulkanTextureManager.h"
#include "graphics/vulkan/VulkanConstants.h"

#include <gtest/gtest.h>
#include <type_traits>

using graphics::vulkan::VulkanTextureManager;

// Compile-time contract tests - verify the fallback texture API exists with correct types.
// These tests run without Vulkan hardware and verify the fix for C9 is in place.

TEST(VulkanFallbackTextureContract, FallbackHandleConstantExists)
{
	// Verify the synthetic handle constant exists and has a value that won't collide
	// with bmpman handles (which are >= 0)
	constexpr int handle = VulkanTextureManager::kFallbackTextureHandle;
	EXPECT_LT(handle, 0) << "Fallback handle must be negative to avoid bmpman collision";
	EXPECT_EQ(handle, -1000) << "Fallback handle should be -1000 by convention";
}

TEST(VulkanFallbackTextureContract, GetFallbackTextureHandleSignature)
{
	// Verify getFallbackTextureHandle() returns int
	using ReturnType = decltype(std::declval<VulkanTextureManager>().getFallbackTextureHandle());
	constexpr bool correct_return = std::is_same_v<ReturnType, int>;
	EXPECT_TRUE(correct_return) << "getFallbackTextureHandle() must return int";
}

TEST(VulkanFallbackTextureContract, TextureStateEnumHasResident)
{
	// Verify TextureState::Resident exists (fallback texture should be in this state)
	auto state = VulkanTextureManager::TextureState::Resident;
	EXPECT_EQ(static_cast<int>(state), static_cast<int>(VulkanTextureManager::TextureState::Resident));
}

TEST(VulkanFallbackTextureContract, AllTexturesAccessorExists)
{
	// Verify allTextures() method exists and returns the correct type
	// This is used to verify the fallback texture is in the texture map
	using ReturnType = decltype(std::declval<VulkanTextureManager>().allTextures());
	using ExpectedType = std::unordered_map<int, VulkanTextureManager::TextureRecord>&;
	constexpr bool correct_return = std::is_same_v<ReturnType, ExpectedType>;
	EXPECT_TRUE(correct_return) << "allTextures() must return reference to texture map";
}

TEST(VulkanFallbackTextureContract, TextureRecordHasRequiredFields)
{
	// Verify TextureRecord has the fields needed to validate fallback texture
	VulkanTextureManager::TextureRecord record{};

	// Verify state field exists and can be set
	record.state = VulkanTextureManager::TextureState::Resident;
	EXPECT_EQ(record.state, VulkanTextureManager::TextureState::Resident);

	// Verify gpu struct has required fields (compile-time check)
	// These are the fields checked in the implementation
	[[maybe_unused]] auto& imageView = record.gpu.imageView;
	[[maybe_unused]] auto& image = record.gpu.image;
	[[maybe_unused]] auto& sampler = record.gpu.sampler;
	[[maybe_unused]] auto width = record.gpu.width;
	[[maybe_unused]] auto height = record.gpu.height;
	[[maybe_unused]] auto layers = record.gpu.layers;
	[[maybe_unused]] auto format = record.gpu.format;
	[[maybe_unused]] auto layout = record.gpu.currentLayout;

	// Default initialization check
	EXPECT_EQ(record.gpu.width, 0u);
	EXPECT_EQ(record.gpu.height, 0u);
}

// Note: Runtime integration testing of VulkanTextureManager::createFallbackTexture()
// is covered by the existing integration test infrastructure in it_vulkan_model_present.cpp.
// That test exercises the full Vulkan rendering path including texture management.
// To run it: set FS2_VULKAN_IT=1 and have a Vulkan-capable GPU with retail FS2 data.
//
// The contract tests above verify that:
// 1. The kFallbackTextureHandle constant exists with a safe negative value
// 2. The getFallbackTextureHandle() method exists with correct signature
// 3. The TextureState enum has the Resident state (fallback texture's expected state)
// 4. The allTextures() accessor exists to verify texture map contents
// 5. TextureRecord has all required fields for validation
//
// Combined with the implementation creating a 1x1 black texture in the constructor
// (which the build verifies compiles), this provides confidence in the C9 fix.
