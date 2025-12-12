#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>
#include <optional>

namespace {

// Simulates VulkanRenderTargets::findDepthFormat() logic.
// This tests the behavioral contract that depth format selection must:
// 1. Require BOTH eDepthStencilAttachment AND eSampledImage features
// 2. Throw if no suitable format exists (no silent fallback)
//
// Bug H10 in REPORT.md had two flaws:
// - Flaw 1: Only checked eDepthStencilAttachment, ignoring eSampledImage
// - Flaw 2: Silent fallback to eD32Sfloat if loop found nothing

// Simulated format feature flags matching Vulkan spec
enum FormatFeature : uint32_t {
	None = 0,
	DepthStencilAttachment = 1 << 0,
	SampledImage = 1 << 1,
};

// Simulated format identifiers
enum DepthFormat {
	Undefined = 0,
	D32SfloatS8Uint = 1,
	D24UnormS8Uint = 2,
	D32Sfloat = 3,
};

// Represents capabilities of a format
struct FormatProperties {
	DepthFormat format;
	uint32_t features;
};

// Simulates the CORRECTED findDepthFormat() logic
// Returns selected format or throws if no suitable format found
DepthFormat findDepthFormat_Correct(const std::vector<FormatProperties>& availableFormats)
{
	const std::vector<DepthFormat> candidates = {
		D32SfloatS8Uint,
		D24UnormS8Uint,
		D32Sfloat,
	};

	// CORRECT: Require BOTH features
	constexpr uint32_t requiredFeatures = DepthStencilAttachment | SampledImage;

	for (auto candidate : candidates) {
		// Find this candidate in available formats
		for (const auto& props : availableFormats) {
			if (props.format == candidate) {
				if ((props.features & requiredFeatures) == requiredFeatures) {
					return candidate;
				}
				break;
			}
		}
	}

	// CORRECT: Throw instead of silent fallback
	throw std::runtime_error("No suitable depth format found with both attachment and sampling support");
}

// Simulates the BUGGY findDepthFormat() logic (before fix)
// This demonstrates Bug H10
DepthFormat findDepthFormat_Buggy(const std::vector<FormatProperties>& availableFormats)
{
	const std::vector<DepthFormat> candidates = {
		D32SfloatS8Uint,
		D24UnormS8Uint,
		D32Sfloat,
	};

	// BUGGY: Only checks DepthStencilAttachment, ignores SampledImage!
	constexpr uint32_t requiredFeatures = DepthStencilAttachment;

	for (auto candidate : candidates) {
		for (const auto& props : availableFormats) {
			if (props.format == candidate) {
				if ((props.features & requiredFeatures) == requiredFeatures) {
					return candidate;
				}
				break;
			}
		}
	}

	// BUGGY: Silent fallback instead of throwing!
	return D32Sfloat;
}

} // namespace

// Test: Format with BOTH features is selected
TEST(VulkanDepthFormatSelection, Scenario_BothFeatures_IsSelected)
{
	std::vector<FormatProperties> formats = {
		{D32SfloatS8Uint, DepthStencilAttachment | SampledImage},
		{D24UnormS8Uint, DepthStencilAttachment | SampledImage},
		{D32Sfloat, DepthStencilAttachment | SampledImage},
	};

	DepthFormat result = findDepthFormat_Correct(formats);

	EXPECT_EQ(result, D32SfloatS8Uint)
		<< "First candidate with both features should be selected";
}

// Test: Format with only attachment feature is NOT selected
TEST(VulkanDepthFormatSelection, Scenario_OnlyAttachment_Throws)
{
	// All formats only support attachment, not sampling
	std::vector<FormatProperties> formats = {
		{D32SfloatS8Uint, DepthStencilAttachment},
		{D24UnormS8Uint, DepthStencilAttachment},
		{D32Sfloat, DepthStencilAttachment},
	};

	EXPECT_THROW(findDepthFormat_Correct(formats), std::runtime_error)
		<< "Must throw when no format supports both features";
}

// Test: Format with only sampling feature is NOT selected
TEST(VulkanDepthFormatSelection, Scenario_OnlySampling_Throws)
{
	// All formats only support sampling, not attachment
	std::vector<FormatProperties> formats = {
		{D32SfloatS8Uint, SampledImage},
		{D24UnormS8Uint, SampledImage},
		{D32Sfloat, SampledImage},
	};

	EXPECT_THROW(findDepthFormat_Correct(formats), std::runtime_error)
		<< "Must throw when no format supports both features";
}

// Test: No suitable format causes exception (no silent fallback)
TEST(VulkanDepthFormatSelection, Scenario_NoSuitableFormat_Throws)
{
	// No formats support required features
	std::vector<FormatProperties> formats = {
		{D32SfloatS8Uint, None},
		{D24UnormS8Uint, None},
		{D32Sfloat, None},
	};

	EXPECT_THROW(findDepthFormat_Correct(formats), std::runtime_error)
		<< "Must throw when no suitable format exists, not silently fallback";
}

// Test: Demonstrates Bug H10 Flaw 1 - only attachment checked
TEST(VulkanDepthFormatSelection, Bug_H10_Flaw1_OnlyAttachmentChecked)
{
	// Format supports attachment but NOT sampling
	// Correct behavior: throw. Buggy behavior: accept.
	std::vector<FormatProperties> formats = {
		{D32SfloatS8Uint, DepthStencilAttachment}, // No SampledImage!
	};

	// Buggy: accepts format that lacks sampling support
	DepthFormat buggyResult = findDepthFormat_Buggy(formats);
	EXPECT_EQ(buggyResult, D32SfloatS8Uint)
		<< "Buggy code accepts format without sampling support";

	// Correct: throws for format that lacks sampling support
	EXPECT_THROW(findDepthFormat_Correct(formats), std::runtime_error)
		<< "Correct code rejects format without sampling support";
}

// Test: Demonstrates Bug H10 Flaw 2 - silent fallback
TEST(VulkanDepthFormatSelection, Bug_H10_Flaw2_SilentFallback)
{
	// No formats in the list at all
	std::vector<FormatProperties> formats = {};

	// Buggy: silently returns D32Sfloat without verification
	DepthFormat buggyResult = findDepthFormat_Buggy(formats);
	EXPECT_EQ(buggyResult, D32Sfloat)
		<< "Buggy code silently falls back instead of throwing";

	// Correct: throws instead of silent fallback
	EXPECT_THROW(findDepthFormat_Correct(formats), std::runtime_error)
		<< "Correct code throws when no format found";
}

// Test: First suitable format is selected (preference order)
TEST(VulkanDepthFormatSelection, Scenario_PreferenceOrder_Respected)
{
	// All formats support both features, but preference order should be respected
	std::vector<FormatProperties> formats = {
		{D32Sfloat, DepthStencilAttachment | SampledImage},
		{D32SfloatS8Uint, DepthStencilAttachment | SampledImage},
		{D24UnormS8Uint, DepthStencilAttachment | SampledImage},
	};

	DepthFormat result = findDepthFormat_Correct(formats);

	// D32SfloatS8Uint is first in candidate list, so it should be selected
	// even though D32Sfloat appears first in available formats
	EXPECT_EQ(result, D32SfloatS8Uint)
		<< "Candidate preference order should take precedence over available order";
}

// Test: Second candidate selected when first lacks features
TEST(VulkanDepthFormatSelection, Scenario_FallbackToSecondCandidate)
{
	// First candidate lacks sampling, second has both features
	std::vector<FormatProperties> formats = {
		{D32SfloatS8Uint, DepthStencilAttachment},           // Missing SampledImage
		{D24UnormS8Uint, DepthStencilAttachment | SampledImage}, // Has both
		{D32Sfloat, DepthStencilAttachment | SampledImage},
	};

	DepthFormat result = findDepthFormat_Correct(formats);

	EXPECT_EQ(result, D24UnormS8Uint)
		<< "Should select second candidate when first lacks required features";
}

// Test: Last candidate selected when only it has required features
TEST(VulkanDepthFormatSelection, Scenario_FallbackToLastCandidate)
{
	// Only D32Sfloat (last candidate) has both features
	std::vector<FormatProperties> formats = {
		{D32SfloatS8Uint, DepthStencilAttachment},
		{D24UnormS8Uint, SampledImage},
		{D32Sfloat, DepthStencilAttachment | SampledImage},
	};

	DepthFormat result = findDepthFormat_Correct(formats);

	EXPECT_EQ(result, D32Sfloat)
		<< "Should select last candidate when it's the only one with both features";
}
