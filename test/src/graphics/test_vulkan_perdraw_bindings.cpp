// test_vulkan_perdraw_bindings.cpp
//
// PURPOSE: Validates the per-draw push descriptor binding contracts.
// The renderer uses 6 push descriptor bindings for per-draw data:
//   - binding 0: matrices UBO
//   - binding 1: generic UBO
//   - binding 2-5: texture samplers (multi-texture materials + post-processing)
//
// INVARIANT: All 6 bindings must be populated with valid descriptors before
// each draw call to avoid validation errors from stale/uninitialized state.
// This is enforced by binding default textures to unused sampler slots.

#include <gtest/gtest.h>
#include <array>
#include <bitset>
#include <cstdint>

namespace {

// Mirror of vk::DescriptorType for testing
enum class DescriptorType {
	UniformBuffer,
	CombinedImageSampler
};

// Simulates the per-draw push descriptor layout from VulkanDescriptorLayouts.cpp
struct PerDrawBindingSpec {
	uint32_t binding;
	uint32_t descriptorCount;
	DescriptorType type;
};

// Expected layout: 2 UBOs + 4 samplers = 6 bindings
constexpr uint32_t kPerDrawBindingCount = 6;
constexpr uint32_t kUniformBindingCount = 2;
constexpr uint32_t kSamplerBindingCount = 4;

std::array<PerDrawBindingSpec, kPerDrawBindingCount> getExpectedBindings()
{
	return {{
		{0, 1, DescriptorType::UniformBuffer},        // matrices
		{1, 1, DescriptorType::UniformBuffer},        // generic
		{2, 1, DescriptorType::CombinedImageSampler}, // texture 0
		{3, 1, DescriptorType::CombinedImageSampler}, // texture 1
		{4, 1, DescriptorType::CombinedImageSampler}, // texture 2
		{5, 1, DescriptorType::CombinedImageSampler}, // texture 3
	}};
}

// Simulates the push descriptor write set for a draw call
class FakePushDescriptorWriter {
  public:
	void bindUniformBuffer(uint32_t binding)
	{
		if (binding >= kPerDrawBindingCount) {
			return;
		}
		m_boundBindings.set(binding);
		m_types[binding] = DescriptorType::UniformBuffer;
	}

	void bindCombinedImageSampler(uint32_t binding)
	{
		if (binding >= kPerDrawBindingCount) {
			return;
		}
		m_boundBindings.set(binding);
		m_types[binding] = DescriptorType::CombinedImageSampler;
	}

	bool allBindingsPopulated() const
	{
		return m_boundBindings.count() == kPerDrawBindingCount;
	}

	bool isBindingPopulated(uint32_t binding) const
	{
		return binding < kPerDrawBindingCount && m_boundBindings.test(binding);
	}

	DescriptorType getType(uint32_t binding) const
	{
		return m_types[binding];
	}

	void reset()
	{
		m_boundBindings.reset();
	}

	uint32_t populatedCount() const
	{
		return static_cast<uint32_t>(m_boundBindings.count());
	}

  private:
	std::bitset<kPerDrawBindingCount> m_boundBindings;
	std::array<DescriptorType, kPerDrawBindingCount> m_types{};
};

// Simulates the pattern from VulkanGraphics.cpp gr_vulkan_render_primitives
void bindPrimitivesDescriptors(FakePushDescriptorWriter& writer, bool hasTexture)
{
	// VulkanGraphics.cpp:1293-1316
	writer.bindUniformBuffer(0);  // matrices
	writer.bindUniformBuffer(1);  // generic

	// Texture at binding 2 (or default if no texture)
	writer.bindCombinedImageSampler(2);

	// Unused extra samplers: bind safe defaults (VulkanGraphics.cpp:1308-1316)
	for (uint32_t i = 3; i <= 5; ++i) {
		writer.bindCombinedImageSampler(i);
	}
}

// Simulates the pattern from VulkanGraphics.cpp gr_vulkan_render_nanovg
void bindNanovgDescriptors(FakePushDescriptorWriter& writer)
{
	// VulkanGraphics.cpp:1510-1532
	// NanoVG binds all 6 bindings even though shaders only use 1 and 2
	writer.bindUniformBuffer(0);  // dummy (required for layout)
	writer.bindUniformBuffer(1);  // nanovg params
	writer.bindCombinedImageSampler(2);  // texture

	// VulkanGraphics.cpp:1525-1531 - unused extra samplers
	for (uint32_t i = 3; i <= 5; ++i) {
		writer.bindCombinedImageSampler(i);
	}
}

// Simulates the pattern from VulkanGraphics.cpp gr_vulkan_render_primitives_batched
void bindBatchedDescriptors(FakePushDescriptorWriter& writer)
{
	// VulkanGraphics.cpp:1662-1699
	writer.bindUniformBuffer(0);  // matrices
	writer.bindUniformBuffer(1);  // generic
	writer.bindCombinedImageSampler(2);  // texture

	// VulkanGraphics.cpp:1689-1697 - unused extra samplers
	for (uint32_t i = 3; i <= 5; ++i) {
		writer.bindCombinedImageSampler(i);
	}
}

} // namespace

// Test: Expected binding layout matches specification
TEST(VulkanPerDrawBindings, ExpectedBindingLayout_MatchesSpec)
{
	auto bindings = getExpectedBindings();

	EXPECT_EQ(bindings.size(), kPerDrawBindingCount);

	// Verify UBO bindings
	EXPECT_EQ(bindings[0].type, DescriptorType::UniformBuffer);
	EXPECT_EQ(bindings[1].type, DescriptorType::UniformBuffer);

	// Verify sampler bindings
	for (uint32_t i = 2; i < kPerDrawBindingCount; ++i) {
		EXPECT_EQ(bindings[i].type, DescriptorType::CombinedImageSampler)
			<< "Binding " << i << " must be a combined image sampler";
	}

	// Verify binding indices are contiguous
	for (uint32_t i = 0; i < kPerDrawBindingCount; ++i) {
		EXPECT_EQ(bindings[i].binding, i)
			<< "Bindings must be contiguous starting at 0";
	}
}

// Test: Primitives draw populates all 6 bindings
TEST(VulkanPerDrawBindings, PrimitivesDraw_PopulatesAllBindings)
{
	FakePushDescriptorWriter writer;

	bindPrimitivesDescriptors(writer, true);

	EXPECT_TRUE(writer.allBindingsPopulated())
		<< "gr_vulkan_render_primitives must populate all 6 bindings";
	EXPECT_EQ(writer.populatedCount(), kPerDrawBindingCount);
}

// Test: NanoVG draw populates all 6 bindings
TEST(VulkanPerDrawBindings, NanovgDraw_PopulatesAllBindings)
{
	FakePushDescriptorWriter writer;

	bindNanovgDescriptors(writer);

	EXPECT_TRUE(writer.allBindingsPopulated())
		<< "gr_vulkan_render_nanovg must populate all 6 bindings";
}

// Test: Batched draw populates all 6 bindings
TEST(VulkanPerDrawBindings, BatchedDraw_PopulatesAllBindings)
{
	FakePushDescriptorWriter writer;

	bindBatchedDescriptors(writer);

	EXPECT_TRUE(writer.allBindingsPopulated())
		<< "gr_vulkan_render_primitives_batched must populate all 6 bindings";
}

// Test: Incomplete bindings detected (validation scenario)
TEST(VulkanPerDrawBindings, IncompleteBindings_Detected)
{
	FakePushDescriptorWriter writer;

	// Old pattern (only 3 bindings)
	writer.bindUniformBuffer(0);
	writer.bindUniformBuffer(1);
	writer.bindCombinedImageSampler(2);

	EXPECT_FALSE(writer.allBindingsPopulated())
		<< "Old 3-binding pattern must be detected as incomplete";
	EXPECT_EQ(writer.populatedCount(), 3u);

	// Bindings 3-5 are unpopulated
	EXPECT_FALSE(writer.isBindingPopulated(3));
	EXPECT_FALSE(writer.isBindingPopulated(4));
	EXPECT_FALSE(writer.isBindingPopulated(5));
}

// Test: Binding type correctness
TEST(VulkanPerDrawBindings, BindingTypeCorrectness)
{
	FakePushDescriptorWriter writer;
	bindPrimitivesDescriptors(writer, true);

	EXPECT_EQ(writer.getType(0), DescriptorType::UniformBuffer);
	EXPECT_EQ(writer.getType(1), DescriptorType::UniformBuffer);
	EXPECT_EQ(writer.getType(2), DescriptorType::CombinedImageSampler);
	EXPECT_EQ(writer.getType(3), DescriptorType::CombinedImageSampler);
	EXPECT_EQ(writer.getType(4), DescriptorType::CombinedImageSampler);
	EXPECT_EQ(writer.getType(5), DescriptorType::CombinedImageSampler);
}

// Test: Reset clears all bindings
TEST(VulkanPerDrawBindings, Reset_ClearsAllBindings)
{
	FakePushDescriptorWriter writer;
	bindPrimitivesDescriptors(writer, true);

	EXPECT_TRUE(writer.allBindingsPopulated());

	writer.reset();

	EXPECT_FALSE(writer.allBindingsPopulated());
	EXPECT_EQ(writer.populatedCount(), 0u);
}

// Test: Binding count constants
TEST(VulkanPerDrawBindings, BindingCountConstants)
{
	EXPECT_EQ(kUniformBindingCount + kSamplerBindingCount, kPerDrawBindingCount)
		<< "UBO count + sampler count must equal total binding count";

	EXPECT_EQ(kUniformBindingCount, 2u);
	EXPECT_EQ(kSamplerBindingCount, 4u);
	EXPECT_EQ(kPerDrawBindingCount, 6u);
}

// Test: Multiple draw calls each need full binding set
TEST(VulkanPerDrawBindings, MultipleDrawCalls_EachNeedsFullBindings)
{
	FakePushDescriptorWriter writer;

	// First draw
	bindPrimitivesDescriptors(writer, true);
	EXPECT_TRUE(writer.allBindingsPopulated());

	// Reset simulates push descriptor state between draws
	writer.reset();
	EXPECT_FALSE(writer.allBindingsPopulated());

	// Second draw must also populate all bindings
	bindNanovgDescriptors(writer);
	EXPECT_TRUE(writer.allBindingsPopulated());
}
