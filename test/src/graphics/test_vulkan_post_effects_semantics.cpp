// test_vulkan_post_effects_semantics.cpp
//
// PURPOSE: Validates the post-effects processing semantics in VulkanRenderer.endSceneTexture().
// This tests the observable behavior of how post effects are applied:
// - Effects are only active when enabled (always_on OR intensity != default_intensity)
// - Identity defaults are applied when no effects are active
// - The doPostEffects flag correctly reflects whether any effect is enabled
//
// INVARIANT: Post-effects semantics must match OpenGL behavior:
// - An effect is enabled if (always_on || intensity != default_intensity)
// - If no effects are enabled, identity defaults are used (saturation=1, brightness=1, etc.)
// - Only enabled effects modify the post_data structure

#include <gtest/gtest.h>
#include <vector>
#include <cmath>

namespace {

// Mirrors graphics::PostEffectUniformType
enum class PostEffectUniformType {
	Invalid,
	NoiseAmount,
	Saturation,
	Brightness,
	Contrast,
	FilmGrain,
	TvStripes,
	Cutoff,
	Dither,
	Tint,
	CustomEffectVEC3A,
	CustomEffectFloatA,
	CustomEffectVEC3B,
	CustomEffectFloatB
};

struct PostEffect {
	PostEffectUniformType uniform_type = PostEffectUniformType::Invalid;
	float intensity = 0.0f;
	float default_intensity = 0.0f;
	bool always_on = false;
	float rgb[3] = {0.0f, 0.0f, 0.0f};
};

// Mirrors graphics::generic_data::post_data
struct PostData {
	float timer = 0.0f;
	float noise_amount = 0.0f;
	float saturation = 1.0f;
	float brightness = 1.0f;
	float contrast = 1.0f;
	float film_grain = 0.0f;
	float tv_stripes = 0.0f;
	float cutoff = 0.0f;
	float dither = 0.0f;
	float tint[3] = {0.0f, 0.0f, 0.0f};
	float custom_effect_vec3_a[3] = {0.0f, 0.0f, 0.0f};
	float custom_effect_float_a = 0.0f;
	float custom_effect_vec3_b[3] = {0.0f, 0.0f, 0.0f};
	float custom_effect_float_b = 0.0f;
};

// Simulates the post-effects processing logic from VulkanRenderer::endSceneTexture
class FakePostEffectsProcessor {
  public:
	void setEffects(std::vector<PostEffect> effects)
	{
		m_effects = std::move(effects);
	}

	// Returns true if any post effect is active
	bool processEffects(PostData& post)
	{
		// Apply identity defaults (always done first)
		post.saturation = 1.0f;
		post.brightness = 1.0f;
		post.contrast = 1.0f;
		post.film_grain = 0.0f;
		post.tv_stripes = 0.0f;
		post.cutoff = 0.0f;
		post.dither = 0.0f;
		post.noise_amount = 0.0f;
		for (int i = 0; i < 3; ++i) {
			post.tint[i] = 0.0f;
			post.custom_effect_vec3_a[i] = 0.0f;
			post.custom_effect_vec3_b[i] = 0.0f;
		}
		post.custom_effect_float_a = 0.0f;
		post.custom_effect_float_b = 0.0f;

		bool doPostEffects = false;

		for (const auto& eff : m_effects) {
			// Match OpenGL semantics: effects are only applied when flagged on
			const bool enabled = eff.always_on || eff.intensity != eff.default_intensity;
			if (!enabled) {
				continue;
			}
			doPostEffects = true;

			switch (eff.uniform_type) {
			case PostEffectUniformType::NoiseAmount:
				post.noise_amount = eff.intensity;
				break;
			case PostEffectUniformType::Saturation:
				post.saturation = eff.intensity;
				break;
			case PostEffectUniformType::Brightness:
				post.brightness = eff.intensity;
				break;
			case PostEffectUniformType::Contrast:
				post.contrast = eff.intensity;
				break;
			case PostEffectUniformType::FilmGrain:
				post.film_grain = eff.intensity;
				break;
			case PostEffectUniformType::TvStripes:
				post.tv_stripes = eff.intensity;
				break;
			case PostEffectUniformType::Cutoff:
				post.cutoff = eff.intensity;
				break;
			case PostEffectUniformType::Dither:
				post.dither = eff.intensity;
				break;
			case PostEffectUniformType::Tint:
				for (int i = 0; i < 3; ++i) {
					post.tint[i] = eff.rgb[i];
				}
				break;
			case PostEffectUniformType::CustomEffectVEC3A:
				for (int i = 0; i < 3; ++i) {
					post.custom_effect_vec3_a[i] = eff.rgb[i];
				}
				break;
			case PostEffectUniformType::CustomEffectFloatA:
				post.custom_effect_float_a = eff.intensity;
				break;
			case PostEffectUniformType::CustomEffectVEC3B:
				for (int i = 0; i < 3; ++i) {
					post.custom_effect_vec3_b[i] = eff.rgb[i];
				}
				break;
			case PostEffectUniformType::CustomEffectFloatB:
				post.custom_effect_float_b = eff.intensity;
				break;
			case PostEffectUniformType::Invalid:
			default:
				break;
			}
		}

		return doPostEffects;
	}

  private:
	std::vector<PostEffect> m_effects;
};

} // namespace

// Test: No effects - identity defaults preserved
TEST(VulkanPostEffectsSemantics, NoEffects_IdentityDefaults)
{
	FakePostEffectsProcessor processor;
	processor.setEffects({});

	PostData post{};
	bool hasEffects = processor.processEffects(post);

	EXPECT_FALSE(hasEffects)
		<< "No effects defined means doPostEffects should be false";
	EXPECT_FLOAT_EQ(post.saturation, 1.0f);
	EXPECT_FLOAT_EQ(post.brightness, 1.0f);
	EXPECT_FLOAT_EQ(post.contrast, 1.0f);
	EXPECT_FLOAT_EQ(post.noise_amount, 0.0f);
}

// Test: Effect at default intensity is NOT active
TEST(VulkanPostEffectsSemantics, EffectAtDefaultIntensity_NotActive)
{
	FakePostEffectsProcessor processor;

	PostEffect eff;
	eff.uniform_type = PostEffectUniformType::Saturation;
	eff.intensity = 0.5f;
	eff.default_intensity = 0.5f; // Same as intensity
	eff.always_on = false;

	processor.setEffects({eff});

	PostData post{};
	bool hasEffects = processor.processEffects(post);

	EXPECT_FALSE(hasEffects)
		<< "Effect at default intensity should not be active";
	EXPECT_FLOAT_EQ(post.saturation, 1.0f)
		<< "Saturation should remain at identity default";
}

// Test: Effect with intensity != default IS active
TEST(VulkanPostEffectsSemantics, EffectIntensityDiffers_IsActive)
{
	FakePostEffectsProcessor processor;

	PostEffect eff;
	eff.uniform_type = PostEffectUniformType::Saturation;
	eff.intensity = 0.75f;
	eff.default_intensity = 0.5f;
	eff.always_on = false;

	processor.setEffects({eff});

	PostData post{};
	bool hasEffects = processor.processEffects(post);

	EXPECT_TRUE(hasEffects)
		<< "Effect with intensity != default should be active";
	EXPECT_FLOAT_EQ(post.saturation, 0.75f)
		<< "Saturation should be set to effect intensity";
}

// Test: always_on effect is active regardless of intensity
TEST(VulkanPostEffectsSemantics, AlwaysOnEffect_ActiveRegardless)
{
	FakePostEffectsProcessor processor;

	PostEffect eff;
	eff.uniform_type = PostEffectUniformType::Brightness;
	eff.intensity = 0.5f;
	eff.default_intensity = 0.5f; // Same as intensity
	eff.always_on = true;

	processor.setEffects({eff});

	PostData post{};
	bool hasEffects = processor.processEffects(post);

	EXPECT_TRUE(hasEffects)
		<< "always_on effect should be active even at default intensity";
	EXPECT_FLOAT_EQ(post.brightness, 0.5f);
}

// Test: Mixed effects - only enabled ones applied
TEST(VulkanPostEffectsSemantics, MixedEffects_OnlyEnabledApplied)
{
	FakePostEffectsProcessor processor;

	std::vector<PostEffect> effects;

	// Effect 1: Saturation at default (disabled)
	PostEffect sat;
	sat.uniform_type = PostEffectUniformType::Saturation;
	sat.intensity = 1.0f;
	sat.default_intensity = 1.0f;
	sat.always_on = false;
	effects.push_back(sat);

	// Effect 2: Brightness modified (enabled)
	PostEffect bright;
	bright.uniform_type = PostEffectUniformType::Brightness;
	bright.intensity = 1.5f;
	bright.default_intensity = 1.0f;
	bright.always_on = false;
	effects.push_back(bright);

	// Effect 3: Contrast always on (enabled)
	PostEffect cont;
	cont.uniform_type = PostEffectUniformType::Contrast;
	cont.intensity = 0.8f;
	cont.default_intensity = 0.8f;
	cont.always_on = true;
	effects.push_back(cont);

	processor.setEffects(effects);

	PostData post{};
	bool hasEffects = processor.processEffects(post);

	EXPECT_TRUE(hasEffects);
	EXPECT_FLOAT_EQ(post.saturation, 1.0f)
		<< "Disabled saturation should remain at identity";
	EXPECT_FLOAT_EQ(post.brightness, 1.5f)
		<< "Enabled brightness should be applied";
	EXPECT_FLOAT_EQ(post.contrast, 0.8f)
		<< "Always-on contrast should be applied";
}

// Test: All effect types can be processed
TEST(VulkanPostEffectsSemantics, AllEffectTypes_Processed)
{
	FakePostEffectsProcessor processor;

	std::vector<PostEffect> effects;

	auto addEffect = [&effects](PostEffectUniformType type, float intensity) {
		PostEffect eff;
		eff.uniform_type = type;
		eff.intensity = intensity;
		eff.default_intensity = 0.0f; // Different from intensity
		eff.always_on = false;
		eff.rgb[0] = intensity;
		eff.rgb[1] = intensity * 0.5f;
		eff.rgb[2] = intensity * 0.25f;
		effects.push_back(eff);
	};

	addEffect(PostEffectUniformType::NoiseAmount, 0.1f);
	addEffect(PostEffectUniformType::Saturation, 0.9f);
	addEffect(PostEffectUniformType::Brightness, 1.1f);
	addEffect(PostEffectUniformType::Contrast, 0.95f);
	addEffect(PostEffectUniformType::FilmGrain, 0.05f);
	addEffect(PostEffectUniformType::TvStripes, 0.02f);
	addEffect(PostEffectUniformType::Cutoff, 0.03f);
	addEffect(PostEffectUniformType::Dither, 0.01f);
	addEffect(PostEffectUniformType::Tint, 0.5f);
	addEffect(PostEffectUniformType::CustomEffectVEC3A, 0.3f);
	addEffect(PostEffectUniformType::CustomEffectFloatA, 0.4f);
	addEffect(PostEffectUniformType::CustomEffectVEC3B, 0.6f);
	addEffect(PostEffectUniformType::CustomEffectFloatB, 0.7f);

	processor.setEffects(effects);

	PostData post{};
	bool hasEffects = processor.processEffects(post);

	EXPECT_TRUE(hasEffects);
	EXPECT_FLOAT_EQ(post.noise_amount, 0.1f);
	EXPECT_FLOAT_EQ(post.saturation, 0.9f);
	EXPECT_FLOAT_EQ(post.brightness, 1.1f);
	EXPECT_FLOAT_EQ(post.contrast, 0.95f);
	EXPECT_FLOAT_EQ(post.film_grain, 0.05f);
	EXPECT_FLOAT_EQ(post.tv_stripes, 0.02f);
	EXPECT_FLOAT_EQ(post.cutoff, 0.03f);
	EXPECT_FLOAT_EQ(post.dither, 0.01f);
	EXPECT_FLOAT_EQ(post.tint[0], 0.5f);
	EXPECT_FLOAT_EQ(post.custom_effect_vec3_a[0], 0.3f);
	EXPECT_FLOAT_EQ(post.custom_effect_float_a, 0.4f);
	EXPECT_FLOAT_EQ(post.custom_effect_vec3_b[0], 0.6f);
	EXPECT_FLOAT_EQ(post.custom_effect_float_b, 0.7f);
}

// Test: Invalid effect type is ignored
TEST(VulkanPostEffectsSemantics, InvalidEffectType_Ignored)
{
	FakePostEffectsProcessor processor;

	PostEffect eff;
	eff.uniform_type = PostEffectUniformType::Invalid;
	eff.intensity = 999.0f;
	eff.default_intensity = 0.0f;
	eff.always_on = true;

	processor.setEffects({eff});

	PostData post{};
	bool hasEffects = processor.processEffects(post);

	// Invalid type is technically "enabled" (always_on) but doesn't modify post_data
	EXPECT_TRUE(hasEffects);
	// All values should be at identity
	EXPECT_FLOAT_EQ(post.saturation, 1.0f);
	EXPECT_FLOAT_EQ(post.brightness, 1.0f);
}

// Test: Effect order - later effects override earlier (last write wins)
TEST(VulkanPostEffectsSemantics, EffectOrder_LastWriteWins)
{
	FakePostEffectsProcessor processor;

	std::vector<PostEffect> effects;

	PostEffect first;
	first.uniform_type = PostEffectUniformType::Saturation;
	first.intensity = 0.5f;
	first.default_intensity = 1.0f;
	first.always_on = false;
	effects.push_back(first);

	PostEffect second;
	second.uniform_type = PostEffectUniformType::Saturation;
	second.intensity = 0.8f;
	second.default_intensity = 1.0f;
	second.always_on = false;
	effects.push_back(second);

	processor.setEffects(effects);

	PostData post{};
	processor.processEffects(post);

	EXPECT_FLOAT_EQ(post.saturation, 0.8f)
		<< "Later effect should override earlier for same uniform type";
}

// Test: Float comparison for intensity != default (exact equality)
TEST(VulkanPostEffectsSemantics, FloatComparison_ExactEquality)
{
	FakePostEffectsProcessor processor;

	// Very small difference - should still be detected as different
	PostEffect eff;
	eff.uniform_type = PostEffectUniformType::Brightness;
	eff.intensity = 1.0f + 1e-7f;
	eff.default_intensity = 1.0f;
	eff.always_on = false;

	processor.setEffects({eff});

	PostData post{};
	bool hasEffects = processor.processEffects(post);

	// The implementation uses != comparison, so any difference enables the effect
	EXPECT_TRUE(hasEffects)
		<< "Any difference in intensity should enable the effect (exact != comparison)";
}

// Test: Tint RGB values are applied correctly
TEST(VulkanPostEffectsSemantics, TintRgb_AppliedCorrectly)
{
	FakePostEffectsProcessor processor;

	PostEffect eff;
	eff.uniform_type = PostEffectUniformType::Tint;
	eff.intensity = 1.0f;
	eff.default_intensity = 0.0f;
	eff.always_on = false;
	eff.rgb[0] = 0.2f;
	eff.rgb[1] = 0.4f;
	eff.rgb[2] = 0.6f;

	processor.setEffects({eff});

	PostData post{};
	processor.processEffects(post);

	EXPECT_FLOAT_EQ(post.tint[0], 0.2f);
	EXPECT_FLOAT_EQ(post.tint[1], 0.4f);
	EXPECT_FLOAT_EQ(post.tint[2], 0.6f);
}

