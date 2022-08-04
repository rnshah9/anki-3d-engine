// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma anki mutator VARIANCE_CLIPPING 0 1
#pragma anki mutator YCBCR 0 1

ANKI_SPECIALIZATION_CONSTANT_F32(VARIANCE_CLIPPING_GAMMA, 0u);
ANKI_SPECIALIZATION_CONSTANT_F32(BLEND_FACTOR, 1u);
ANKI_SPECIALIZATION_CONSTANT_UVEC2(FB_SIZE, 2u);

#include <AnKi/Shaders/Functions.glsl>
#include <AnKi/Shaders/PackFunctions.glsl>
#include <AnKi/Shaders/TonemappingFunctions.glsl>

layout(set = 0, binding = 0) uniform sampler u_linearAnyClampSampler;
layout(set = 0, binding = 1) uniform texture2D u_depthRt;
layout(set = 0, binding = 2) uniform ANKI_RP texture2D u_inputRt;
layout(set = 0, binding = 3) uniform ANKI_RP texture2D u_historyRt;
layout(set = 0, binding = 4) uniform texture2D u_motionVectorsTex;

const U32 TONEMAPPING_SET = 0u;
const U32 TONEMAPPING_BINDING = 5u;
#include <AnKi/Shaders/TonemappingResources.glsl>

#if defined(ANKI_COMPUTE_SHADER)
layout(set = 0, binding = 6) writeonly uniform image2D u_outImg;
layout(set = 0, binding = 7) writeonly uniform image2D u_tonemappedImg;

const UVec2 WORKGROUP_SIZE = UVec2(8, 8);
layout(local_size_x = WORKGROUP_SIZE.x, local_size_y = WORKGROUP_SIZE.y, local_size_z = 1) in;
#else
layout(location = 0) in Vec2 in_uv;
layout(location = 0) out Vec3 out_color;
layout(location = 1) out Vec3 out_tonemappedColor;
#endif

#if YCBCR
#	define sample(s, uv) rgbToYCbCr(textureLod(s, u_linearAnyClampSampler, uv, 0.0).rgb)
#	define sampleOffset(s, uv, x, y) \
		rgbToYCbCr(textureLodOffset(sampler2D(s, u_linearAnyClampSampler), uv, 0.0, IVec2(x, y)).rgb)
#else
#	define sample(s, uv) textureLod(s, u_linearAnyClampSampler, uv, 0.0).rgb
#	define sampleOffset(s, uv, x, y) textureLodOffset(sampler2D(s, u_linearAnyClampSampler), uv, 0.0, IVec2(x, y)).rgb
#endif

void main()
{
#if defined(ANKI_COMPUTE_SHADER)
	if(skipOutOfBoundsInvocations(WORKGROUP_SIZE, FB_SIZE))
	{
		return;
	}

	const Vec2 uv = (Vec2(gl_GlobalInvocationID.xy) + 0.5) / Vec2(FB_SIZE);
#else
	const Vec2 uv = in_uv;
#endif

	const F32 depth = textureLod(u_depthRt, u_linearAnyClampSampler, uv, 0.0).r;

	// Get prev uv coords
	const Vec2 oldUv = uv + textureLod(u_motionVectorsTex, u_linearAnyClampSampler, uv, 0.0).rg;

	// Read textures
	Vec3 historyCol = sample(u_historyRt, oldUv);
	const Vec3 crntCol = sample(u_inputRt, uv);

	// Remove ghosting by clamping the history color to neighbour's AABB
	const Vec3 near0 = sampleOffset(u_inputRt, uv, 1, 0);
	const Vec3 near1 = sampleOffset(u_inputRt, uv, 0, 1);
	const Vec3 near2 = sampleOffset(u_inputRt, uv, -1, 0);
	const Vec3 near3 = sampleOffset(u_inputRt, uv, 0, -1);

#if VARIANCE_CLIPPING
	const Vec3 m1 = crntCol + near0 + near1 + near2 + near3;
	const Vec3 m2 = crntCol * crntCol + near0 * near0 + near1 * near1 + near2 * near2 + near3 * near3;

	const Vec3 mu = m1 / 5.0;
	const Vec3 sigma = sqrt(m2 / 5.0 - mu * mu);

	const Vec3 boxMin = mu - VARIANCE_CLIPPING_GAMMA * sigma;
	const Vec3 boxMax = mu + VARIANCE_CLIPPING_GAMMA * sigma;
#else
	const Vec3 boxMin = min(crntCol, min(near0, min(near1, min(near2, near3))));
	const Vec3 boxMax = max(crntCol, max(near0, max(near1, max(near2, near3))));
#endif

	historyCol = clamp(historyCol, boxMin, boxMax);

	// Remove jitter (T. Lottes)
#if YCBCR
	const F32 lum0 = crntCol.r;
	const F32 lum1 = historyCol.r;
	const F32 maxLum = boxMax.r;
#else
	const F32 lum0 = computeLuminance(reinhardTonemap(crntCol));
	const F32 lum1 = computeLuminance(reinhardTonemap(historyCol));
	const F32 maxLum = 1.0;
#endif

	F32 diff = abs(lum0 - lum1) / max(lum0, max(lum1, maxLum + EPSILON));
	diff = 1.0 - diff;
	diff = diff * diff;
	const F32 feedback = mix(0.0, BLEND_FACTOR, diff);

	// Write result
	Vec3 outColor = mix(historyCol, crntCol, feedback);
#if YCBCR
	outColor = yCbCrToRgb(outColor);
#endif
	const Vec3 tonemapped = linearToSRgb(tonemap(outColor, readExposureAndAverageLuminance().x));
#if defined(ANKI_COMPUTE_SHADER)
	imageStore(u_outImg, IVec2(gl_GlobalInvocationID.xy), Vec4(outColor, 0.0));
	imageStore(u_tonemappedImg, IVec2(gl_GlobalInvocationID.xy), Vec4(tonemapped, 0.0));
#else
	out_color = outColor;
	out_tonemappedColor = tonemapped;
#endif
}
