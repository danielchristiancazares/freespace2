// Shared decal helper code for Vulkan decal passes.
// Mirrors the OpenGL decal shader logic but adapts for Vulkan depth range (NDC z in [0,1]).

#include "normals.sdr"
#include "gamma.sdr"
#include "lighting.glsl"

layout(set = 1, binding = 3) uniform sampler2D DepthBuffer;

layout(binding = 2) uniform sampler2DArray diffuseMap;
layout(binding = 3) uniform sampler2DArray glowMap;
layout(binding = 4) uniform sampler2DArray normalMap;

layout(binding = 0, std140) uniform decalGlobalData {
	mat4 viewMatrix;
	mat4 projMatrix;
	mat4 invViewMatrix;
	mat4 invProjMatrix;

	vec2 viewportSize;
	float pad0;
	float pad1;
} uGlobals;

layout(binding = 1, std140) uniform decalInfoData {
	int diffuse_index;
	int glow_index;
	int normal_index;
	int diffuse_blend_mode;

	int glow_blend_mode;
	float pad0;
	float pad1;
	float pad2;
} uInfo;

layout(location = 0) flat in mat4 invModelMatrix;
layout(location = 4) flat in vec3 decalDirection;
layout(location = 5) flat in float normal_angle_cutoff;
layout(location = 6) flat in float angle_fade_start;
layout(location = 7) flat in float alpha_scale;

vec3 computeViewPosition(ivec2 pixelCoord)
{
	vec4 clipSpaceLocation;
	vec2 normalizedCoord = vec2(pixelCoord) / uGlobals.viewportSize;

	clipSpaceLocation.xy = normalizedCoord * 2.0 - 1.0;
	// Vulkan NDC depth range is [0,1], so do not remap to [-1,1].
	clipSpaceLocation.z = texelFetch(DepthBuffer, pixelCoord, 0).r;
	clipSpaceLocation.w = 1.0;

	vec4 homogenousLocation = uGlobals.invProjMatrix * clipSpaceLocation;
	return homogenousLocation.xyz / homogenousLocation.w;
}

vec2 getDecalTexCoord(vec3 view_pos, inout float alpha)
{
	vec4 object_pos = invModelMatrix * uGlobals.invViewMatrix * vec4(view_pos, 1.0);

	bvec3 invalidComponents = greaterThan(abs(object_pos.xyz), vec3(0.5));
	bvec4 nanComponents = isnan(object_pos);
	if (any(invalidComponents) || any(nanComponents)) {
		discard;
	}

	// Fade out when close to decal volume edge (z axis)
	alpha = alpha * (1.0 - smoothstep(0.4, 0.5, abs(object_pos.z)));
	return object_pos.xy + 0.5;
}

vec3 getSurfaceNormal(vec3 frag_position, inout float alpha, out vec3 binormal, out vec3 tangent)
{
	// Derive the surface normal from screen-space derivatives of the reconstructed view-space position.
	vec3 pos_dx = dFdx(frag_position);
	vec3 pos_dy = dFdy(frag_position);

	vec3 normal = normalize(cross(pos_dx, pos_dy));
	binormal = normalize(pos_dx);
	tangent = normalize(pos_dy);

	// Angle cutoff against decal direction
	float angle = acos(dot(normal, decalDirection));
	if (angle > normal_angle_cutoff) {
		discard;
	}

	alpha = alpha * (1.0 - smoothstep(angle_fade_start, normal_angle_cutoff, angle));
	return normal;
}

// Compute per-pass contributions. Some passes may ignore outputs they don't need.
void computeDecalOutputs(ivec2 pixelCoord, out vec4 diffuse_out, out vec4 emissive_out, out vec3 normal_out)
{
	vec3 frag_position = computeViewPosition(pixelCoord);

	float alpha = alpha_scale;
	vec2 tex_coord = getDecalTexCoord(frag_position, alpha);

	vec3 binormal;
	vec3 tangent;
	vec3 normal = getSurfaceNormal(frag_position, alpha, binormal, tangent);

	diffuse_out = vec4(0.0);
	emissive_out = vec4(0.0);
	normal_out = vec3(0.0);

	if (uInfo.diffuse_index >= 0) {
		vec4 color = texture(diffuseMap, vec3(tex_coord, float(uInfo.diffuse_index)));
		color.rgb = srgb_to_linear(color.rgb);

		if (uInfo.diffuse_blend_mode == 0) {
			diffuse_out = vec4(color.rgb, color.a * alpha);
		} else {
			// Additive
			diffuse_out = vec4(color.rgb * alpha, 1.0);
		}
	}

	if (uInfo.glow_index >= 0) {
		vec4 color = texture(glowMap, vec3(tex_coord, float(uInfo.glow_index)));
		color.rgb = srgb_to_linear(color.rgb) * GLOW_MAP_SRGB_MULTIPLIER;
		color.rgb *= GLOW_MAP_INTENSITY;

		if (uInfo.glow_blend_mode == 0) {
			emissive_out = vec4(color.rgb, color.a * alpha);
		} else {
			emissive_out.rgb += color.rgb * alpha;
		}
	}

	if (uInfo.normal_index >= 0) {
		vec3 decalNormal = unpackNormal(texture(normalMap, vec3(tex_coord, float(uInfo.normal_index))).ag);

		mat3 tangentToView;
		tangentToView[0] = tangent;
		tangentToView[1] = binormal;
		tangentToView[2] = normal;

		normal_out = tangentToView * decalNormal * alpha;
	}
}


