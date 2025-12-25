#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragOut0;

// Input is RGBL (luma in alpha) from the prepass.
layout(binding = 2) uniform sampler2D tex0;

layout(binding = 1, std140) uniform genericData {
	float rt_w;
	float rt_h;
	float pad0;
	float pad1;
} u;

// Fixed shader configuration (Vulkan backend).
// We keep these compile-time to avoid runtime-variant shader compilation.
#define FXAA_GLSL_120 0
#define FXAA_GLSL_130 1
#define FXAA_GATHER4_ALPHA 0

// Match OpenGL's FXAA "Medium" defaults (see gropenglpostprocessing.cpp).
#define FXAA_QUALITY_PRESET 26
#define FXAA_QUALITY_EDGE_THRESHOLD (1.0/12.0)
#define FXAA_QUALITY_EDGE_THRESHOLD_MIN (1.0/24.0)
#define FXAA_QUALITY_SUBPIX 0.33

// FXAA shader core (based on the engine's legacy `fxaa-f.sdr`) with Vulkan-friendly interface.
// Input is RGBL where L is stored in alpha; therefore FxaaLuma reads .w.

#define FXAA_EARLY_EXIT 1
#define FXAA_DISCARD 1
#define FXAA_FAST_PIXEL_OFFSET 1

#ifndef FXAA_GATHER4_ALPHA
	#define FXAA_GATHER4_ALPHA 0
#endif

#if (FXAA_QUALITY_PRESET == 10)
	#define FXAA_QUALITY_PS 3
	#define FXAA_QUALITY_P0 1.5
	#define FXAA_QUALITY_P1 3.0
	#define FXAA_QUALITY_P2 12.0
#endif
#if (FXAA_QUALITY_PRESET == 11)
	#define FXAA_QUALITY_PS 4
	#define FXAA_QUALITY_P0 1.0
	#define FXAA_QUALITY_P1 1.5
	#define FXAA_QUALITY_P2 3.0
	#define FXAA_QUALITY_P3 12.0
#endif
#if (FXAA_QUALITY_PRESET == 12)
	#define FXAA_QUALITY_PS 5
	#define FXAA_QUALITY_P0 1.0
	#define FXAA_QUALITY_P1 1.5
	#define FXAA_QUALITY_P2 2.0
	#define FXAA_QUALITY_P3 4.0
	#define FXAA_QUALITY_P4 12.0
#endif
#if (FXAA_QUALITY_PRESET == 13)
	#define FXAA_QUALITY_PS 6
	#define FXAA_QUALITY_P0 1.0
	#define FXAA_QUALITY_P1 1.5
	#define FXAA_QUALITY_P2 2.0
	#define FXAA_QUALITY_P3 2.0
	#define FXAA_QUALITY_P4 4.0
	#define FXAA_QUALITY_P5 12.0
#endif
#if (FXAA_QUALITY_PRESET == 14)
	#define FXAA_QUALITY_PS 7
	#define FXAA_QUALITY_P0 1.0
	#define FXAA_QUALITY_P1 1.5
	#define FXAA_QUALITY_P2 2.0
	#define FXAA_QUALITY_P3 2.0
	#define FXAA_QUALITY_P4 2.0
	#define FXAA_QUALITY_P5 4.0
	#define FXAA_QUALITY_P6 12.0
#endif
#if (FXAA_QUALITY_PRESET == 25)
	#define FXAA_QUALITY_PS 8
	#define FXAA_QUALITY_P0 1.0
	#define FXAA_QUALITY_P1 1.5
	#define FXAA_QUALITY_P2 2.0
	#define FXAA_QUALITY_P3 2.0
	#define FXAA_QUALITY_P4 2.0
	#define FXAA_QUALITY_P5 2.0
	#define FXAA_QUALITY_P6 4.0
	#define FXAA_QUALITY_P7 8.0
#endif
#if (FXAA_QUALITY_PRESET == 26)
	#define FXAA_QUALITY_PS 9
	#define FXAA_QUALITY_P0 1.0
	#define FXAA_QUALITY_P1 1.5
	#define FXAA_QUALITY_P2 2.0
	#define FXAA_QUALITY_P3 2.0
	#define FXAA_QUALITY_P4 2.0
	#define FXAA_QUALITY_P5 2.0
	#define FXAA_QUALITY_P6 2.0
	#define FXAA_QUALITY_P7 4.0
	#define FXAA_QUALITY_P8 8.0
#endif
#if (FXAA_QUALITY_PRESET == 27)
	#define FXAA_QUALITY_PS 10
	#define FXAA_QUALITY_P0 1.0
	#define FXAA_QUALITY_P1 1.5
	#define FXAA_QUALITY_P2 2.0
	#define FXAA_QUALITY_P3 2.0
	#define FXAA_QUALITY_P4 2.0
	#define FXAA_QUALITY_P5 2.0
	#define FXAA_QUALITY_P6 2.0
	#define FXAA_QUALITY_P7 2.0
	#define FXAA_QUALITY_P8 4.0
	#define FXAA_QUALITY_P9 8.0
#endif
#if (FXAA_QUALITY_PRESET == 28)
	#define FXAA_QUALITY_PS 11
	#define FXAA_QUALITY_P0 1.0
	#define FXAA_QUALITY_P1 1.5
	#define FXAA_QUALITY_P2 2.0
	#define FXAA_QUALITY_P3 2.0
	#define FXAA_QUALITY_P4 2.0
	#define FXAA_QUALITY_P5 2.0
	#define FXAA_QUALITY_P6 2.0
	#define FXAA_QUALITY_P7 2.0
	#define FXAA_QUALITY_P8 2.0
	#define FXAA_QUALITY_P9 4.0
	#define FXAA_QUALITY_P10 8.0
#endif
#if (FXAA_QUALITY_PRESET == 39)
	#define FXAA_QUALITY_PS 12
	#define FXAA_QUALITY_P0 1.0
	#define FXAA_QUALITY_P1 1.0
	#define FXAA_QUALITY_P2 1.0
	#define FXAA_QUALITY_P3 1.0
	#define FXAA_QUALITY_P4 1.0
	#define FXAA_QUALITY_P5 1.5
	#define FXAA_QUALITY_P6 2.0
	#define FXAA_QUALITY_P7 2.0
	#define FXAA_QUALITY_P8 2.0
	#define FXAA_QUALITY_P9 2.0
	#define FXAA_QUALITY_P10 4.0
	#define FXAA_QUALITY_P11 8.0
#endif

#define FxaaBool bool
#define FxaaDiscard discard
#define FxaaFloat float
#define FxaaFloat2 vec2
#define FxaaFloat3 vec3
#define FxaaFloat4 vec4
#define FxaaHalf float
#define FxaaHalf2 vec2
#define FxaaHalf3 vec3
#define FxaaHalf4 vec4
#define FxaaInt2 ivec2
#define FxaaSat(x) clamp(x, 0.0, 1.0)
#define FxaaTex sampler2D

#define FxaaTexTop(t, p) textureLod(t, p, 0.0)
#define FxaaTexOff(t, p, o, r) textureLodOffset(t, p, 0.0, o)

// Luma is stored in alpha (RGBL input).
FxaaFloat FxaaLuma(FxaaFloat4 rgba) { return rgba.w; }

FxaaFloat4 FxaaPixelShader(
	FxaaFloat2 pos,
	FxaaTex tex,
	FxaaFloat2 fxaaQualityRcpFrame,
	FxaaFloat fxaaQualitySubpix,
	FxaaFloat fxaaQualityEdgeThreshold,
	FxaaFloat fxaaQualityEdgeThresholdMin
) {
	FxaaFloat2 posM;
	posM.x = pos.x;
	posM.y = pos.y;

	FxaaFloat4 rgbyM = FxaaTexTop(tex, posM);
	#define lumaM rgbyM.w
	FxaaFloat lumaS = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2( 0, 1), fxaaQualityRcpFrame.xy));
	FxaaFloat lumaE = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2( 1, 0), fxaaQualityRcpFrame.xy));
	FxaaFloat lumaN = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2( 0,-1), fxaaQualityRcpFrame.xy));
	FxaaFloat lumaW = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2(-1, 0), fxaaQualityRcpFrame.xy));

	FxaaFloat maxSM = max(lumaS, lumaM);
	FxaaFloat minSM = min(lumaS, lumaM);
	FxaaFloat maxESM = max(lumaE, maxSM);
	FxaaFloat minESM = min(lumaE, minSM);
	FxaaFloat maxWN = max(lumaN, lumaW);
	FxaaFloat minWN = min(lumaN, lumaW);
	FxaaFloat rangeMax = max(maxWN, maxESM);
	FxaaFloat rangeMin = min(minWN, minESM);
	FxaaFloat rangeMaxScaled = rangeMax * fxaaQualityEdgeThreshold;
	FxaaFloat range = rangeMax - rangeMin;
	FxaaFloat rangeMaxClamped = max(fxaaQualityEdgeThresholdMin, rangeMaxScaled);
	FxaaBool earlyExit = range < rangeMaxClamped;
	if (earlyExit)
		#if (FXAA_DISCARD == 1)
			FxaaDiscard;
		#else
			return rgbyM;
		#endif

	FxaaFloat lumaNW = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2(-1,-1), fxaaQualityRcpFrame.xy));
	FxaaFloat lumaSE = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2( 1, 1), fxaaQualityRcpFrame.xy));
	FxaaFloat lumaNE = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2( 1,-1), fxaaQualityRcpFrame.xy));
	FxaaFloat lumaSW = FxaaLuma(FxaaTexOff(tex, posM, FxaaInt2(-1, 1), fxaaQualityRcpFrame.xy));

	FxaaFloat lumaNS = lumaN + lumaS;
	FxaaFloat lumaWE = lumaW + lumaE;
	FxaaFloat subpixRcpRange = 1.0/range;
	FxaaFloat subpixNSWE = lumaNS + lumaWE;
	FxaaFloat edgeHorz1 = (-2.0 * lumaM) + lumaNS;
	FxaaFloat edgeVert1 = (-2.0 * lumaM) + lumaWE;
	FxaaFloat lumaNESE = lumaNE + lumaSE;
	FxaaFloat lumaNWNE = lumaNW + lumaNE;
	FxaaFloat edgeHorz2 = (-2.0 * lumaE) + lumaNESE;
	FxaaFloat edgeVert2 = (-2.0 * lumaN) + lumaNWNE;
	FxaaFloat lumaNWSW = lumaNW + lumaSW;
	FxaaFloat lumaSWSE = lumaSW + lumaSE;
	FxaaFloat edgeHorz4 = (abs(edgeHorz1) * 2.0) + abs(edgeHorz2);
	FxaaFloat edgeVert4 = (abs(edgeVert1) * 2.0) + abs(edgeVert2);
	FxaaFloat edgeHorz3 = (-2.0 * lumaW) + lumaNWSW;
	FxaaFloat edgeVert3 = (-2.0 * lumaS) + lumaSWSE;
	FxaaFloat edgeHorz = abs(edgeHorz3) + edgeHorz4;
	FxaaFloat edgeVert = abs(edgeVert3) + edgeVert4;
	FxaaFloat subpixNWSWNESE = lumaNWSW + lumaNESE;
	FxaaFloat lengthSign = fxaaQualityRcpFrame.x;
	FxaaBool horzSpan = edgeHorz >= edgeVert;
	FxaaFloat subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
	if (!horzSpan) lumaN = lumaW;
	if (!horzSpan) lumaS = lumaE;
	if (horzSpan) lengthSign = fxaaQualityRcpFrame.y;
	FxaaFloat subpixB = (subpixA * (1.0/12.0)) - lumaM;
	FxaaFloat subpixC = FxaaSat(abs(subpixB) * subpixRcpRange);
	FxaaFloat subpixD = ((-2.0)*subpixC) + 3.0;
	FxaaFloat subpixE = subpixC * subpixC;
	FxaaFloat subpixF = subpixD * subpixE;
	FxaaFloat lumaL = lumaN + lumaM;
	FxaaFloat lumaR = lumaS + lumaM;
	FxaaFloat lumaB = lumaW + lumaM;
	FxaaFloat lumaT = lumaE + lumaM;
	FxaaFloat lumaLmn = min(lumaN, lumaM);
	FxaaFloat lumaLmx = max(lumaN, lumaM);
	FxaaFloat lumaRmn = min(lumaS, lumaM);
	FxaaFloat lumaRmx = max(lumaS, lumaM);
	FxaaFloat lumaBmn = min(lumaW, lumaM);
	FxaaFloat lumaBmx = max(lumaW, lumaM);
	FxaaFloat lumaTmn = min(lumaE, lumaM);
	FxaaFloat lumaTmx = max(lumaE, lumaM);
	lumaLmn = min(lumaLmn, lumaW);
	lumaLmx = max(lumaLmx, lumaW);
	lumaRmn = min(lumaRmn, lumaE);
	lumaRmx = max(lumaRmx, lumaE);
	lumaBmn = min(lumaBmn, lumaS);
	lumaBmx = max(lumaBmx, lumaS);
	lumaTmn = min(lumaTmn, lumaN);
	lumaTmx = max(lumaTmx, lumaN);
	FxaaFloat lumaMmn = min(lumaLmn, lumaRmn);
	FxaaFloat lumaMmx = max(lumaLmx, lumaRmx);
	lumaMmn = min(lumaMmn, lumaBmn);
	lumaMmx = max(lumaMmx, lumaBmx);
	lumaMmn = min(lumaMmn, lumaTmn);
	lumaMmx = max(lumaMmx, lumaTmx);
	FxaaFloat2 posB;
	posB.x = posM.x;
	posB.y = posM.y;
	FxaaFloat2 posN;
	posN.x = posM.x;
	posN.y = posM.y;
	FxaaFloat2 posP;
	posP.x = posM.x;
	posP.y = posM.y;
	FxaaFloat2 offNP;
	if (horzSpan) offNP.x = 0.0;
	if (horzSpan) offNP.y = fxaaQualityRcpFrame.y;
	if (!horzSpan) offNP.x = fxaaQualityRcpFrame.x;
	if (!horzSpan) offNP.y = 0.0;
	if (horzSpan) posB.x += 0.0;
	if (horzSpan) posB.y += offNP.y * 0.5;
	if (!horzSpan) posB.x += offNP.x * 0.5;
	if (!horzSpan) posB.y += 0.0;
	posN.x = posB.x - offNP.x * FXAA_QUALITY_P0;
	posN.y = posB.y - offNP.y * FXAA_QUALITY_P0;
	posP.x = posB.x + offNP.x * FXAA_QUALITY_P0;
	posP.y = posB.y + offNP.y * FXAA_QUALITY_P0;
	FxaaFloat lumaEndN = FxaaLuma(FxaaTexTop(tex, posN));
	FxaaFloat lumaEndP = FxaaLuma(FxaaTexTop(tex, posP));
	FxaaBool lumaMLTZero = lumaM < 0.0;
	lumaEndN -= lumaM;
	lumaEndP -= lumaM;
	FxaaBool doneN = abs(lumaEndN) >= abs(lumaEndP);
	FxaaBool doneP = abs(lumaEndP) >= abs(lumaEndN);
	if (doneN) posP = posB;
	if (doneP) posN = posB;
	if (!doneN) posN.x -= offNP.x * FXAA_QUALITY_P1;
	if (!doneN) posN.y -= offNP.y * FXAA_QUALITY_P1;
	if (!doneP) posP.x += offNP.x * FXAA_QUALITY_P1;
	if (!doneP) posP.y += offNP.y * FXAA_QUALITY_P1;
	FxaaBool doneNP = (!doneN) || (!doneP);
	#if (FXAA_QUALITY_PS > 3)
	if (doneNP) {
		if (!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN));
		if (!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP));
		if (!doneN) lumaEndN -= lumaM;
		if (!doneP) lumaEndP -= lumaM;
		doneN = abs(lumaEndN) >= abs(lumaEndP);
		doneP = abs(lumaEndP) >= abs(lumaEndN);
		if (doneN) posP = posB;
		if (doneP) posN = posB;
		if (!doneN) posN.x -= offNP.x * FXAA_QUALITY_P2;
		if (!doneN) posN.y -= offNP.y * FXAA_QUALITY_P2;
		if (!doneP) posP.x += offNP.x * FXAA_QUALITY_P2;
		if (!doneP) posP.y += offNP.y * FXAA_QUALITY_P2;
		doneNP = (!doneN) || (!doneP);
	}
	#endif
	#if (FXAA_QUALITY_PS > 4)
	if (doneNP) {
		if (!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN));
		if (!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP));
		if (!doneN) lumaEndN -= lumaM;
		if (!doneP) lumaEndP -= lumaM;
		doneN = abs(lumaEndN) >= abs(lumaEndP);
		doneP = abs(lumaEndP) >= abs(lumaEndN);
		if (doneN) posP = posB;
		if (doneP) posN = posB;
		if (!doneN) posN.x -= offNP.x * FXAA_QUALITY_P3;
		if (!doneN) posN.y -= offNP.y * FXAA_QUALITY_P3;
		if (!doneP) posP.x += offNP.x * FXAA_QUALITY_P3;
		if (!doneP) posP.y += offNP.y * FXAA_QUALITY_P3;
		doneNP = (!doneN) || (!doneP);
	}
	#endif
	#if (FXAA_QUALITY_PS > 5)
	if (doneNP) {
		if (!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN));
		if (!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP));
		if (!doneN) lumaEndN -= lumaM;
		if (!doneP) lumaEndP -= lumaM;
		doneN = abs(lumaEndN) >= abs(lumaEndP);
		doneP = abs(lumaEndP) >= abs(lumaEndN);
		if (doneN) posP = posB;
		if (doneP) posN = posB;
		if (!doneN) posN.x -= offNP.x * FXAA_QUALITY_P4;
		if (!doneN) posN.y -= offNP.y * FXAA_QUALITY_P4;
		if (!doneP) posP.x += offNP.x * FXAA_QUALITY_P4;
		if (!doneP) posP.y += offNP.y * FXAA_QUALITY_P4;
		doneNP = (!doneN) || (!doneP);
	}
	#endif
	#if (FXAA_QUALITY_PS > 6)
	if (doneNP) {
		if (!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN));
		if (!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP));
		if (!doneN) lumaEndN -= lumaM;
		if (!doneP) lumaEndP -= lumaM;
		doneN = abs(lumaEndN) >= abs(lumaEndP);
		doneP = abs(lumaEndP) >= abs(lumaEndN);
		if (doneN) posP = posB;
		if (doneP) posN = posB;
		if (!doneN) posN.x -= offNP.x * FXAA_QUALITY_P5;
		if (!doneN) posN.y -= offNP.y * FXAA_QUALITY_P5;
		if (!doneP) posP.x += offNP.x * FXAA_QUALITY_P5;
		if (!doneP) posP.y += offNP.y * FXAA_QUALITY_P5;
		doneNP = (!doneN) || (!doneP);
	}
	#endif
	#if (FXAA_QUALITY_PS > 7)
	if (doneNP) {
		if (!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN));
		if (!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP));
		if (!doneN) lumaEndN -= lumaM;
		if (!doneP) lumaEndP -= lumaM;
		doneN = abs(lumaEndN) >= abs(lumaEndP);
		doneP = abs(lumaEndP) >= abs(lumaEndN);
		if (doneN) posP = posB;
		if (doneP) posN = posB;
		if (!doneN) posN.x -= offNP.x * FXAA_QUALITY_P6;
		if (!doneN) posN.y -= offNP.y * FXAA_QUALITY_P6;
		if (!doneP) posP.x += offNP.x * FXAA_QUALITY_P6;
		if (!doneP) posP.y += offNP.y * FXAA_QUALITY_P6;
		doneNP = (!doneN) || (!doneP);
	}
	#endif
	#if (FXAA_QUALITY_PS > 8)
	if (doneNP) {
		if (!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN));
		if (!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP));
		if (!doneN) lumaEndN -= lumaM;
		if (!doneP) lumaEndP -= lumaM;
		doneN = abs(lumaEndN) >= abs(lumaEndP);
		doneP = abs(lumaEndP) >= abs(lumaEndN);
		if (doneN) posP = posB;
		if (doneP) posN = posB;
		if (!doneN) posN.x -= offNP.x * FXAA_QUALITY_P7;
		if (!doneN) posN.y -= offNP.y * FXAA_QUALITY_P7;
		if (!doneP) posP.x += offNP.x * FXAA_QUALITY_P7;
		if (!doneP) posP.y += offNP.y * FXAA_QUALITY_P7;
		doneNP = (!doneN) || (!doneP);
	}
	#endif
	#if (FXAA_QUALITY_PS > 9)
	if (doneNP) {
		if (!doneN) lumaEndN = FxaaLuma(FxaaTexTop(tex, posN));
		if (!doneP) lumaEndP = FxaaLuma(FxaaTexTop(tex, posP));
		if (!doneN) lumaEndN -= lumaM;
		if (!doneP) lumaEndP -= lumaM;
		doneN = abs(lumaEndN) >= abs(lumaEndP);
		doneP = abs(lumaEndP) >= abs(lumaEndN);
		if (doneN) posP = posB;
		if (doneP) posN = posB;
		if (!doneN) posN.x -= offNP.x * FXAA_QUALITY_P8;
		if (!doneN) posN.y -= offNP.y * FXAA_QUALITY_P8;
		if (!doneP) posP.x += offNP.x * FXAA_QUALITY_P8;
		if (!doneP) posP.y += offNP.y * FXAA_QUALITY_P8;
		doneNP = (!doneN) || (!doneP);
	}
	#endif
	FxaaFloat dstN = posM.x - posN.x;
	FxaaFloat dstP = posP.x - posM.x;
	if (!horzSpan) dstN = posM.y - posN.y;
	if (!horzSpan) dstP = posP.y - posM.y;
	FxaaBool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
	FxaaFloat spanLength = (dstP + dstN);
	FxaaBool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
	FxaaFloat spanLengthRcp = 1.0/spanLength;
	FxaaBool directionN = dstN < dstP;
	FxaaFloat dst = min(dstN, dstP);
	FxaaBool goodSpan = directionN ? goodSpanN : goodSpanP;
	FxaaFloat subpixG = subpixF * subpixF;
	FxaaFloat pixelOffset = (dst * (-spanLengthRcp)) + 0.5;
	FxaaFloat subpixH = subpixG * fxaaQualitySubpix;
	FxaaFloat pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
	FxaaFloat pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
	if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;
	if (horzSpan) posM.y += pixelOffsetSubpix * lengthSign;
	#if (FXAA_DISCARD == 1)
		return FxaaTexTop(tex, posM);
	#else
		return FxaaFloat4(FxaaTexTop(tex, posM).xyz, lumaM);
	#endif
}

void main()
{
	vec2 rcpFrame = vec2(1.0 / u.rt_w, 1.0 / u.rt_h);
	fragOut0 = FxaaPixelShader(
		fragTexCoord,
		tex0,
		rcpFrame,
		FXAA_QUALITY_SUBPIX,
		FXAA_QUALITY_EDGE_THRESHOLD,
		FXAA_QUALITY_EDGE_THRESHOLD_MIN);
}


