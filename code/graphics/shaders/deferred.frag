#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "lighting.glsl"

layout(location = 0) out vec4 fragOut0;

// Global deferred inputs (set=1): bound by VulkanRenderer::bindDeferredGlobalDescriptors()
layout(set = 1, binding = 0) uniform sampler2D ColorBuffer;
layout(set = 1, binding = 1) uniform sampler2D NormalBuffer;
layout(set = 1, binding = 2) uniform sampler2D PositionBuffer;
layout(set = 1, binding = 3) uniform sampler2D DepthBuffer; // currently unused
layout(set = 1, binding = 4) uniform sampler2D SpecularBuffer;
layout(set = 1, binding = 5) uniform sampler2D EmissiveBuffer;

// Per-draw UBOs (set=0): bound via push descriptors
layout(binding = 0, std140) uniform matrixData {
	mat4 modelViewMatrix;
	mat4 projMatrix;
};

layout(binding = 1, std140) uniform lightData {
	vec3 diffuseLightColor;
	float coneAngle;

	vec3 lightDir;
	float coneInnerAngle;

	vec3 coneDir;
	uint dualCone;

	vec3 scale;
	float lightRadius;

	int lightType;
	uint enable_shadows;

	float sourceRadius;
};

vec2 getScreenUV()
{
	// Use G-buffer size directly (avoids requiring screen size uniforms)
	ivec2 sz = textureSize(ColorBuffer, 0);
	return gl_FragCoord.xy / vec2(sz);
}

// Nearest point sphere and tube light calculations taken from
// "Real Shading in Unreal Engine 4" by Brian Karis, Epic Games.
vec3 ExpandLightSize(in vec3 lightDir, in vec3 reflectDir)
{
	vec3 centerToRay = max(dot(lightDir, reflectDir), sourceRadius) * reflectDir - lightDir;
	return lightDir + centerToRay * clamp(sourceRadius / length(centerToRay), 0.0, 1.0);
}

void GetLightInfo(vec3 position,
	in float alpha,
	in vec3 reflectDir,
	out vec3 lightDirOut,
	out float attenuation,
	out float area_normalisation)
{
	if (lightType == LT_DIRECTIONAL) {
		lightDirOut = normalize(lightDir);
		attenuation = 1.0;
		area_normalisation = 1.0;
		return;
	}

	// Positional light sources: light position is encoded in modelViewMatrix translation
	vec3 lightPosition = modelViewMatrix[3].xyz;

	if (lightType == LT_POINT) {
		lightDirOut = lightPosition - position.xyz;
		float dist = length(lightDirOut);

		// Expand light size for area lights (sourceRadius)
		lightDirOut = ExpandLightSize(lightDirOut, reflectDir);
		dist = length(lightDirOut);

		// Energy conservation term
		float alpha_adjust = clamp(alpha + (sourceRadius / (2.0 * dist)), 0.0, 1.0);
		area_normalisation = alpha / alpha_adjust;
		area_normalisation *= area_normalisation;

		if (dist > lightRadius) {
			discard;
		}
		attenuation = 1.0 - clamp(sqrt(dist / lightRadius), 0.0, 1.0);
	}
	else if (lightType == LT_TUBE) {
		// Tube light: beam direction derived from the transformed -Z axis
		vec3 beamVec = vec3(modelViewMatrix * vec4(0.0, 0.0, -scale.z, 0.0));
		vec3 beamDir = normalize(beamVec);

		// The actual lighting segment is shorter than the cylinder mesh to allow falloff at the ends.
		vec3 adjustedLightPos = lightPosition - (beamDir * lightRadius);
		vec3 adjustedBeamVec = beamVec - 2.0 * lightRadius * beamDir;
		float beamLength = length(adjustedBeamVec);
		vec3 sourceDir = adjustedLightPos - position.xyz;

		// Get point on beam nearest to the reflection ray.
		vec3 a_t = reflectDir;
		vec3 b_t = beamDir;
		vec3 b_0 = sourceDir;
		vec3 c = cross(a_t, b_t);
		vec3 d = b_0;
		vec3 r = d - a_t * dot(d, a_t) - c * dot(d, c);
		float neardist = dot(r, r) / dot(b_t, r);

		// Move along the beam by the distance we calculated
		lightDirOut = sourceDir - beamDir * clamp(neardist, 0.0, beamLength);

		// Treat the nearest point like a sphere light for sourceRadius expansion
		lightDirOut = ExpandLightSize(lightDirOut, reflectDir);
		float dist = length(lightDirOut);

		// Energy conservation term (line light: no square)
		float alpha_adjust = min(alpha + (sourceRadius / (2.0 * dist)), 1.0);
		area_normalisation = alpha / alpha_adjust;

		if (dist > lightRadius) {
			discard;
		}
		attenuation = 1.0 - clamp(sqrt(dist / lightRadius), 0.0, 1.0);
	}
	else if (lightType == LT_CONE) {
		lightDirOut = lightPosition - position.xyz;
		float coneDot = dot(normalize(-lightDirOut), coneDir);
		float dist = length(lightDirOut);

		area_normalisation = 1.0;
		attenuation = 1.0 - clamp(sqrt(dist / lightRadius), 0.0, 1.0);

		// Dual cone option matches OpenGL shader behavior
		if (dualCone != 0u) {
			if (abs(coneDot) < coneAngle) {
				discard;
			} else {
				attenuation *= smoothstep(coneAngle, coneInnerAngle, abs(coneDot));
			}
		} else {
			if (coneDot < coneAngle) {
				discard;
			} else {
				attenuation *= smoothstep(coneAngle, coneInnerAngle, coneDot);
			}
		}
	}
	else if (lightType == LT_AMBIENT) {
		// Ambient handled separately in main()
		lightDirOut = vec3(0.0, 0.0, 1.0);
		attenuation = 1.0;
		area_normalisation = 1.0;
	}
	else {
		// Unknown light type: render nothing
		discard;
	}

	attenuation *= attenuation;
	lightDirOut = normalize(lightDirOut);
}

void main()
{
	vec2 screenPos = getScreenUV();

	vec4 position_buffer = texture(PositionBuffer, screenPos);
	vec3 position = position_buffer.xyz;

	// If no geometry wrote this pixel, position buffer stays at clear (0,0,0,0)
	if (dot(position, position) < 1.0e-8) {
		discard;
	}

	vec4 diffuse = texture(ColorBuffer, screenPos);
	vec3 diffColor = diffuse.rgb;

	vec4 normalData = texture(NormalBuffer, screenPos);
	vec3 normal = normalize(normalData.xyz);

	// #region agent log - H26, H29, H31: Diagnostic shader output
	// H26: Check for Float16 position overflow (infinity)
	bool hasInfPos = isinf(position.x) || isinf(position.y) || isinf(position.z);
	// H29: Check for zero normals
	bool hasZeroNormal = dot(normal, normal) < 0.01;
	// H31: Check for black albedo
	bool hasBlackAlbedo = dot(diffColor, diffColor) < 0.001;
	
	// Diagnostic output: Red=infinity, Green=zero normal, Blue=black albedo, Yellow=multiple issues
	if (hasInfPos || hasZeroNormal || hasBlackAlbedo) {
		if (hasInfPos && hasZeroNormal && hasBlackAlbedo) {
			fragOut0 = vec4(1.0, 1.0, 0.0, 1.0); // Yellow = all issues
		} else if (hasInfPos) {
			fragOut0 = vec4(1.0, 0.0, 0.0, 1.0); // Red = infinity
		} else if (hasZeroNormal) {
			fragOut0 = vec4(0.0, 1.0, 0.0, 1.0); // Green = zero normal
		} else if (hasBlackAlbedo) {
			fragOut0 = vec4(0.0, 0.0, 1.0, 1.0); // Blue = black albedo
		}
		return;
	}
	// #endregion agent log

	vec3 outRgb = vec3(0.0);

	if (lightType == LT_AMBIENT) {
		float ao = position_buffer.w;
		// Emissive applied exactly once. Ambient is first fullscreen pass
		// with blend disabled - correct place for one-time contributions.
		vec3 emissive = texture(EmissiveBuffer, screenPos).rgb;
		outRgb = diffuseLightColor * diffColor * ao + emissive;
	} else {
		// Specular always valid: model.frag writes dielectric defaults.
		vec4 specColor = texture(SpecularBuffer, screenPos);

		// Normal alpha is gloss in legacy path (placeholder: model.frag writes 1.0).
		float gloss = normalData.a;
		float roughness = clamp(1.0 - gloss, 0.0, 1.0);
		float alpha = roughness * roughness;

		vec3 eyeDir = normalize(-position);
		vec3 reflectDir = reflect(-eyeDir, normal);

		vec3 L;
		float attenuation;
		float area_norm;
		GetLightInfo(position, alpha, reflectDir, L, attenuation, area_norm);

		vec3 halfVec = normalize(L + eyeDir);
		float NdotL = clamp(dot(normal, L), 0.0, 1.0);

		float fresnel = specColor.a;
		outRgb =
			computeLighting(specColor.rgb, diffColor, L, normal.xyz, halfVec, eyeDir, roughness, fresnel, NdotL)
			* diffuseLightColor
			* attenuation
			* area_norm;
		
		// #region agent log - H24, H30: Check if lighting calculation produces zero
		// If output is near-zero, output diagnostic color to identify which term is zero
		if (dot(outRgb, outRgb) < 0.001) {
			// Magenta = lighting calculation produced zero
			// Check individual terms
			vec3 lightingTerm = computeLighting(specColor.rgb, diffColor, L, normal.xyz, halfVec, eyeDir, roughness, fresnel, NdotL);
			if (dot(lightingTerm, lightingTerm) < 0.001) {
				fragOut0 = vec4(1.0, 0.0, 1.0, 1.0); // Magenta = computeLighting zero
			} else if (attenuation < 0.001) {
				fragOut0 = vec4(1.0, 0.5, 0.0, 1.0); // Orange = attenuation zero
			} else if (dot(diffuseLightColor, diffuseLightColor) < 0.001) {
				fragOut0 = vec4(0.5, 0.0, 1.0, 1.0); // Purple = light color zero
			} else {
				fragOut0 = vec4(1.0, 1.0, 0.0, 1.0); // Yellow = area_norm or other term zero
			}
			return;
		}
		// #endregion agent log
	}

	fragOut0 = max(vec4(outRgb, 1.0), vec4(0.0));
}
