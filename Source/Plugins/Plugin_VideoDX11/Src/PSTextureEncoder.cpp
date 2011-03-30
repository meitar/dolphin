// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "PSTextureEncoder.h"

#include "D3DBase.h"
#include "D3DShader.h"
#include "GfxState.h"
#include "BPMemory.h"
#include "FramebufferManager.h"
#include "Render.h"
#include "HW/Memmap.h"
#include "TextureCache.h"

// "Static mode" will compile a new EFB encoder shader for every combination of
// encoding configurations. It's compatible with Shader Model 4.

// "Dynamic mode" will use the dynamic-linking feature of Shader Model 5. Only
// one shader needs to be compiled.

// Unfortunately, the June 2010 DirectX SDK includes a broken HLSL compiler
// which cripples dynamic linking for us.
// See <http://www.gamedev.net/topic/587232-dx11-dynamic-linking-compilation-warnings/>.
// Dynamic mode is disabled for now. To enable it, uncomment the line below.

//#define USE_DYNAMIC_MODE

// FIXME: When Microsoft fixes their HLSL compiler, make Dolphin enable dynamic
// mode on Shader Model 5-compatible cards.

namespace DX11
{
	
union EFBEncodeParams
{
	struct
	{
		FLOAT NumHalfCacheLinesX;
		FLOAT NumBlocksY;
		FLOAT PosX;
		FLOAT PosY;
		FLOAT TexLeft;
		FLOAT TexTop;
		FLOAT TexRight;
		FLOAT TexBottom;
	};
	// Constant buffers must be a multiple of 16 bytes in size.
	u8 pad[32]; // Pad to the next multiple of 16 bytes
};

static const char EFB_ENCODE_VS[] =
"// dolphin-emu EFB encoder vertex shader\n"

"cbuffer cbParams : register(b0)\n"
"{\n"
	"struct\n" // Should match EFBEncodeParams above
	"{\n"
		"float NumHalfCacheLinesX;\n"
		"float NumBlocksY;\n"
		"float PosX;\n" // Upper-left corner of source
		"float PosY;\n"
		"float TexLeft;\n" // Rectangle within EFBTexture representing the actual EFB (normalized)
		"float TexTop;\n"
		"float TexRight;\n"
		"float TexBottom;\n"
	"} Params;\n"
"}\n"

"struct Output\n"
"{\n"
	"float4 Pos : SV_Position;\n"
	"float2 Coord : ENCODECOORD;\n"
"};\n"

"Output main(in float2 Pos : POSITION)\n"
"{\n"
	"Output result;\n"
	"result.Pos = float4(2*Pos.x-1, -2*Pos.y+1, 0.0, 1.0);\n"
	"result.Coord = Pos * float2(Params.NumHalfCacheLinesX, Params.NumBlocksY);\n"
	"return result;\n"
"}\n"
;

static const char EFB_ENCODE_PS[] =
"// dolphin-emu EFB encoder pixel shader\n"

// Input

"cbuffer cbParams : register(b0)\n"
"{\n"
	"struct\n" // Should match EFBEncodeParams above
	"{\n"
		"float NumHalfCacheLinesX;\n"
		"float NumBlocksY;\n"
		"float PosX;\n" // Upper-left corner of source
		"float PosY;\n"
		"float TexLeft;\n" // Rectangle within EFBTexture representing the actual EFB (normalized)
		"float TexTop;\n"
		"float TexRight;\n"
		"float TexBottom;\n"
	"} Params;\n"
"}\n"

"Texture2D EFBTexture : register(t0);\n"
"sampler EFBSampler : register(s0);\n"

// Constants

"static const float2 INV_EFB_DIMS = float2(1.0/640.0, 1.0/528.0);\n"

// FIXME: Is this correct?
"static const float3 INTENSITY_COEFFS = float3(0.257, 0.504, 0.098);\n"
"static const float INTENSITY_ADD = 16.0/255.0;\n"

// Utility functions

"uint4 Swap4_32(uint4 v) {\n"
	"return (((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000));\n"
"}\n"

"uint4 UINT4_8888_BE(uint4 a, uint4 b, uint4 c, uint4 d) {\n"
	"return (d << 24) | (c << 16) | (b << 8) | a;\n"
"}\n"

"uint UINT_44444444_BE(uint a, uint b, uint c, uint d, uint e, uint f, uint g, uint h) {\n"
	"return (g << 28) | (h << 24) | (e << 20) | (f << 16) | (c << 12) | (d << 8) | (a << 4) | b;\n"
"}\n"

"uint UINT_1555(uint a, uint b, uint c, uint d) {\n"
	"return (a << 15) | (b << 10) | (c << 5) | d;\n"
"}\n"

"uint UINT_3444(uint a, uint b, uint c, uint d) {\n"
	"return (a << 12) | (b << 8) | (c << 4) | d;\n"
"}\n"

"uint UINT_565(uint a, uint b, uint c) {\n"
	"return (a << 11) | (b << 5) | c;\n"
"}\n"

"uint UINT_1616(uint a, uint b) {\n"
	"return (a << 16) | b;\n"
"}\n"

"uint EncodeRGB5A3(float4 pixel) {\n"
	"if (pixel.a >= 224.0/255.0) {\n"
		// Encode to ARGB1555
		"return UINT_1555(1, pixel.r*31, pixel.g*31, pixel.b*31);\n"
	"} else {\n"
		// Encode to ARGB3444
		"return UINT_3444(pixel.a*7, pixel.r*15, pixel.g*15, pixel.b*15);\n"
	"}\n"
"}\n"

"uint EncodeRGB565(float4 pixel) {\n"
	"return UINT_565(pixel.r*31, pixel.g*63, pixel.b*31);\n"
"}\n"

"float2 CalcTexCoord(float2 coord)\n"
"{\n"
	// Add 0.5,0.5 to sample from the center of the EFB pixel
	"float2 efbCoord = coord + float2(0.5,0.5);\n"
	"return lerp(float2(Params.TexLeft,Params.TexTop), float2(Params.TexRight,Params.TexBottom), efbCoord * INV_EFB_DIMS);\n"
"}\n"

// Interface and classes for different source formats

"float4 Fetch_0(float2 coord)\n"
"{\n"
	"float2 texCoord = CalcTexCoord(coord);\n"
	"float4 result = EFBTexture.Sample(EFBSampler, texCoord);\n"
	"result.a = 1.0;\n"
	"return result;\n"
"}\n"

"float4 Fetch_1(float2 coord)\n"
"{\n"
	"float2 texCoord = CalcTexCoord(coord);\n"
	"return EFBTexture.Sample(EFBSampler, texCoord);\n"
"}\n"

"float4 Fetch_2(float2 coord)\n"
"{\n"
	"float2 texCoord = CalcTexCoord(coord);\n"
	"float4 result = EFBTexture.Sample(EFBSampler, texCoord);\n"
	"result.a = 1.0;\n"
	"return result;\n"
"}\n"

"float4 Fetch_3(float2 coord)\n"
"{\n"
	"float2 texCoord = CalcTexCoord(coord);\n"

	"uint depth24 = 0xFFFFFF * EFBTexture.Sample(EFBSampler, texCoord).r;\n"
	"uint4 bytes = uint4(\n"
		"(depth24 >> 16) & 0xFF,\n" // r
		"(depth24 >> 8) & 0xFF,\n"  // g
		"depth24 & 0xFF,\n"         // b
		"255);\n"                   // a
	"return bytes / 255.0;\n"
"}\n"

"#ifdef DYNAMIC_MODE\n"
"interface iFetch\n"
"{\n"
	"float4 Fetch(float2 coord);\n"
"};\n"

// Source format 0
"class cFetch_0 : iFetch\n"
"{\n"
	"float4 Fetch(float2 coord)\n"
	"{ return Fetch_0(coord); }\n"
"};\n"


// Source format 1
"class cFetch_1 : iFetch\n"
"{\n"
	"float4 Fetch(float2 coord)\n"
	"{ return Fetch_1(coord); }\n"
"};\n"

// Source format 2
"class cFetch_2 : iFetch\n"
"{\n"
	"float4 Fetch(float2 coord)\n"
	"{ return Fetch_2(coord); }\n"
"};\n"

// Source format 3
"class cFetch_3 : iFetch\n"
"{\n"
	"float4 Fetch(float2 coord)\n"
	"{ return Fetch_3(coord); }\n"
"};\n"

// Declare fetch interface; must be set by application
"iFetch g_fetch;\n"
"#define IMP_FETCH g_fetch.Fetch\n"

"#endif\n" // #ifdef DYNAMIC_MODE

"#ifndef IMP_FETCH\n"
"#error No Fetch specified\n"
"#endif\n"

// Interface and classes for different intensity settings (on or off)

"float4 Intensity_0(float4 sample)\n"
"{\n"
	"return sample;\n"
"}\n"

"float4 Intensity_1(float4 sample)\n"
"{\n"
	"sample.r = dot(INTENSITY_COEFFS, sample.rgb) + INTENSITY_ADD;\n"
	// FIXME: Is this correct? What happens if you use one of the non-R
	// formats with intensity on?
	"sample = sample.rrrr;\n"
	"return sample;\n"
"}\n"

"#ifdef DYNAMIC_MODE\n"
"interface iIntensity\n"
"{\n"
	"float4 Intensity(float4 sample);\n"
"};\n"

// Intensity off
"class cIntensity_0 : iIntensity\n"
"{\n"
	"float4 Intensity(float4 sample)\n"
	"{ return Intensity_0(sample); }\n"
"};\n"

// Intensity on
"class cIntensity_1 : iIntensity\n"
"{\n"
	"float4 Intensity(float4 sample)\n"
	"{ return Intensity_1(sample); }\n"
"};\n"

// Declare intensity interface; must be set by application
"iIntensity g_intensity;\n"
"#define IMP_INTENSITY g_intensity.Intensity\n"

"#endif\n" // #ifdef DYNAMIC_MODE

"#ifndef IMP_INTENSITY\n"
"#error No Intensity specified\n"
"#endif\n"


// Interface and classes for different scale/filter settings (on or off)

"float4 ScaledFetch_0(float2 coord)\n"
"{\n"
	"return IMP_FETCH(float2(Params.PosX,Params.PosY) + coord);\n"
"}\n"

"float4 ScaledFetch_1(float2 coord)\n"
"{\n"
	"float2 ul = float2(Params.PosX,Params.PosY) + 2*coord;\n"
	"float4 sample0 = IMP_FETCH(ul+float2(0,0));\n"
	"float4 sample1 = IMP_FETCH(ul+float2(1,0));\n"
	"float4 sample2 = IMP_FETCH(ul+float2(0,1));\n"
	"float4 sample3 = IMP_FETCH(ul+float2(1,1));\n"
	// Average all four samples together
	// FIXME: Is this correct?
	"return 0.25 * (sample0+sample1+sample2+sample3);\n"
"}\n"

"#ifdef DYNAMIC_MODE\n"
"interface iScaledFetch\n"
"{\n"
	"float4 ScaledFetch(float2 coord);\n"
"};\n"

// Scale off
"class cScaledFetch_0 : iScaledFetch\n"
"{\n"
	"float4 ScaledFetch(float2 coord)\n"
	"{ return ScaledFetch_0(coord); }\n"
"};\n"

// Scale on
"class cScaledFetch_1 : iScaledFetch\n"
"{\n"
	"float4 ScaledFetch(float2 coord)\n"
	"{ return ScaledFetch_1(coord); }\n"
"};\n"

// Declare scaled fetch interface; must be set by application code
"iScaledFetch g_scaledFetch;\n"
"#define IMP_SCALEDFETCH g_scaledFetch.ScaledFetch\n"

"#endif\n" // #ifdef DYNAMIC_MODE

"#ifndef IMP_SCALEDFETCH\n"
"#error No ScaledFetch specified\n"
"#endif\n"

// Main EFB-sampling function: performs all steps of fetching pixels, scaling,
// applying intensity function

"float4 SampleEFB(float2 coord)\n"
"{\n"
	// FIXME: Does intensity happen before or after scaling? Or does
	// it matter?
	"float4 sample = IMP_SCALEDFETCH(coord);\n"
	"return IMP_INTENSITY(sample);\n"
"}\n"

// Interfaces and classes for different destination formats

"uint4 Generate_0(float2 cacheCoord)\n" // R4
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(8,8);\n"
	"float2 subBlockUL = blockUL + float2(0, 4*(cacheCoord.x%2));\n"

	"float4 sample[32];\n"
	"for (uint y = 0; y < 4; ++y) {\n"
		"for (uint x = 0; x < 8; ++x) {\n"
			"sample[y*8+x] = SampleEFB(subBlockUL+float2(x,y));\n"
		"}\n"
	"}\n"
		
	"uint dw[4];\n"
	"for (uint i = 0; i < 4; ++i) {\n"
		"dw[i] = UINT_44444444_BE(\n"
			"15*sample[8*i+0].r,\n"
			"15*sample[8*i+1].r,\n"
			"15*sample[8*i+2].r,\n"
			"15*sample[8*i+3].r,\n"
			"15*sample[8*i+4].r,\n"
			"15*sample[8*i+5].r,\n"
			"15*sample[8*i+6].r,\n"
			"15*sample[8*i+7].r\n"
			");\n"
	"}\n"

	"return uint4(dw[0], dw[1], dw[2], dw[3]);\n"
"}\n"

// FIXME: Untested
"uint4 Generate_1(float2 cacheCoord)\n" // R8 (FIXME: Duplicate of R8 below?)
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(8,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"

	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(4,0));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(5,0));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(6,0));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(7,0));\n"
	"float4 sample8 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample9 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sampleA = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sampleB = SampleEFB(subBlockUL+float2(3,1));\n"
	"float4 sampleC = SampleEFB(subBlockUL+float2(4,1));\n"
	"float4 sampleD = SampleEFB(subBlockUL+float2(5,1));\n"
	"float4 sampleE = SampleEFB(subBlockUL+float2(6,1));\n"
	"float4 sampleF = SampleEFB(subBlockUL+float2(7,1));\n"

	"uint4 dw4 = UINT4_8888_BE(\n"
		"255*float4(sample0.r, sample4.r, sample8.r, sampleC.r),\n"
		"255*float4(sample1.r, sample5.r, sample9.r, sampleD.r),\n"
		"255*float4(sample2.r, sample6.r, sampleA.r, sampleE.r),\n"
		"255*float4(sample3.r, sample7.r, sampleB.r, sampleF.r)\n"
		");\n"
	
	"return dw4;\n"
"}\n"

// FIXME: Untested
"uint4 Generate_2(float2 cacheCoord)\n" // A4 R4
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(8,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"
	
	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(4,0));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(5,0));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(6,0));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(7,0));\n"
	"float4 sample8 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample9 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sampleA = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sampleB = SampleEFB(subBlockUL+float2(3,1));\n"
	"float4 sampleC = SampleEFB(subBlockUL+float2(4,1));\n"
	"float4 sampleD = SampleEFB(subBlockUL+float2(5,1));\n"
	"float4 sampleE = SampleEFB(subBlockUL+float2(6,1));\n"
	"float4 sampleF = SampleEFB(subBlockUL+float2(7,1));\n"

	"uint dw0 = UINT_44444444_BE(\n"
		"15*sample0.a, 15*sample0.r,\n"
		"15*sample1.a, 15*sample1.r,\n"
		"15*sample2.a, 15*sample2.r,\n"
		"15*sample3.a, 15*sample3.r\n"
		");\n"
	"uint dw1 = UINT_44444444_BE(\n"
		"15*sample4.a, 15*sample4.r,\n"
		"15*sample5.a, 15*sample5.r,\n"
		"15*sample6.a, 15*sample6.r,\n"
		"15*sample7.a, 15*sample7.r\n"
		");\n"
	"uint dw2 = UINT_44444444_BE(\n"
		"15*sample8.a, 15*sample8.r,\n"
		"15*sample9.a, 15*sample9.r,\n"
		"15*sampleA.a, 15*sampleA.r,\n"
		"15*sampleB.a, 15*sampleB.r\n"
		");\n"
	"uint dw3 = UINT_44444444_BE(\n"
		"15*sampleC.a, 15*sampleC.r,\n"
		"15*sampleD.a, 15*sampleD.r,\n"
		"15*sampleE.a, 15*sampleE.r,\n"
		"15*sampleF.a, 15*sampleF.r\n"
		");\n"
	
	"return uint4(dw0, dw1, dw2, dw3);\n"
"}\n"

// FIXME: Untested
"uint4 Generate_3(float2 cacheCoord)\n" // A8 R8
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(4,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"

	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(3,1));\n"

	"uint4 dw4 = UINT4_8888_BE(\n"
		"255*float4(sample0.a, sample2.a, sample4.a, sample6.a),\n"
		"255*float4(sample0.r, sample2.r, sample4.r, sample6.r),\n"
		"255*float4(sample1.a, sample3.a, sample5.a, sample7.a),\n"
		"255*float4(sample1.r, sample3.r, sample5.r, sample7.r)\n"
		");\n"
	
	"return dw4;\n"
"}\n"

"uint4 Generate_4(float2 cacheCoord)\n" // R5 G6 B5
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(4,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"

	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(3,1));\n"
		
	"uint dw0 = UINT_1616(EncodeRGB565(sample0), EncodeRGB565(sample1));\n"
	"uint dw1 = UINT_1616(EncodeRGB565(sample2), EncodeRGB565(sample3));\n"
	"uint dw2 = UINT_1616(EncodeRGB565(sample4), EncodeRGB565(sample5));\n"
	"uint dw3 = UINT_1616(EncodeRGB565(sample6), EncodeRGB565(sample7));\n"

	"return Swap4_32(uint4(dw0, dw1, dw2, dw3));\n"
"}\n"

"uint4 Generate_5(float2 cacheCoord)\n" // 1 R5 G5 B5 or 0 A3 R4 G4 G4
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(4,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"

	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(3,1));\n"
		
	"uint dw0 = UINT_1616(EncodeRGB5A3(sample0), EncodeRGB5A3(sample1));\n"
	"uint dw1 = UINT_1616(EncodeRGB5A3(sample2), EncodeRGB5A3(sample3));\n"
	"uint dw2 = UINT_1616(EncodeRGB5A3(sample4), EncodeRGB5A3(sample5));\n"
	"uint dw3 = UINT_1616(EncodeRGB5A3(sample6), EncodeRGB5A3(sample7));\n"
	
	"return Swap4_32(uint4(dw0, dw1, dw2, dw3));\n"
"}\n"

"uint4 Generate_6(float2 cacheCoord)\n" // A8 R8 A8 R8 | G8 B8 G8 B8
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(4,1));\n"

	"float2 blockUL = blockCoord * float2(4,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"

	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(3,1));\n"

	"uint4 dw4;\n"
	"if (cacheCoord.x % 4 < 2)\n"
	"{\n"
		// First cache line gets AR
		"dw4 = UINT4_8888_BE(\n"
			"255*float4(sample0.a, sample2.a, sample4.a, sample6.a),\n"
			"255*float4(sample0.r, sample2.r, sample4.r, sample6.r),\n"
			"255*float4(sample1.a, sample3.a, sample5.a, sample7.a),\n"
			"255*float4(sample1.r, sample3.r, sample5.r, sample7.r)\n"
			");\n"
	"}\n"
	"else\n"
	"{\n"
		// Second cache line gets GB
		"dw4 = UINT4_8888_BE(\n"
			"255*float4(sample0.g, sample2.g, sample4.g, sample6.g),\n"
			"255*float4(sample0.b, sample2.b, sample4.b, sample6.b),\n"
			"255*float4(sample1.g, sample3.g, sample5.g, sample7.g),\n"
			"255*float4(sample1.b, sample3.b, sample5.b, sample7.b)\n"
			");\n"
	"}\n"
	
	"return dw4;\n"
"}\n"

"uint4 Generate_7(float2 cacheCoord)\n" // A8
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(8,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"
	
	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(4,0));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(5,0));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(6,0));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(7,0));\n"
	"float4 sample8 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample9 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sampleA = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sampleB = SampleEFB(subBlockUL+float2(3,1));\n"
	"float4 sampleC = SampleEFB(subBlockUL+float2(4,1));\n"
	"float4 sampleD = SampleEFB(subBlockUL+float2(5,1));\n"
	"float4 sampleE = SampleEFB(subBlockUL+float2(6,1));\n"
	"float4 sampleF = SampleEFB(subBlockUL+float2(7,1));\n"

	"uint4 dw4 = UINT4_8888_BE(\n"
		"255*float4(sample0.a, sample4.a, sample8.a, sampleC.a),\n"
		"255*float4(sample1.a, sample5.a, sample9.a, sampleD.a),\n"
		"255*float4(sample2.a, sample6.a, sampleA.a, sampleE.a),\n"
		"255*float4(sample3.a, sample7.a, sampleB.a, sampleF.a)\n"
		");\n"
	
	"return dw4;\n"
"}\n"

"uint4 Generate_8(float2 cacheCoord)\n" // R8
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(8,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"

	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(4,0));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(5,0));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(6,0));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(7,0));\n"
	"float4 sample8 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample9 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sampleA = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sampleB = SampleEFB(subBlockUL+float2(3,1));\n"
	"float4 sampleC = SampleEFB(subBlockUL+float2(4,1));\n"
	"float4 sampleD = SampleEFB(subBlockUL+float2(5,1));\n"
	"float4 sampleE = SampleEFB(subBlockUL+float2(6,1));\n"
	"float4 sampleF = SampleEFB(subBlockUL+float2(7,1));\n"

	"uint4 dw4 = UINT4_8888_BE(\n"
		"255*float4(sample0.r, sample4.r, sample8.r, sampleC.r),\n"
		"255*float4(sample1.r, sample5.r, sample9.r, sampleD.r),\n"
		"255*float4(sample2.r, sample6.r, sampleA.r, sampleE.r),\n"
		"255*float4(sample3.r, sample7.r, sampleB.r, sampleF.r)\n"
		");\n"
	
	"return dw4;\n"
"}\n"

// FIXME: Untested
"uint4 Generate_9(float2 cacheCoord)\n" // G8
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(8,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"

	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(4,0));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(5,0));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(6,0));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(7,0));\n"
	"float4 sample8 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample9 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sampleA = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sampleB = SampleEFB(subBlockUL+float2(3,1));\n"
	"float4 sampleC = SampleEFB(subBlockUL+float2(4,1));\n"
	"float4 sampleD = SampleEFB(subBlockUL+float2(5,1));\n"
	"float4 sampleE = SampleEFB(subBlockUL+float2(6,1));\n"
	"float4 sampleF = SampleEFB(subBlockUL+float2(7,1));\n"

	"uint4 dw4 = UINT4_8888_BE(\n"
		"255*float4(sample0.g, sample4.g, sample8.g, sampleC.g),\n"
		"255*float4(sample1.g, sample5.g, sample9.g, sampleD.g),\n"
		"255*float4(sample2.g, sample6.g, sampleA.g, sampleE.g),\n"
		"255*float4(sample3.g, sample7.g, sampleB.g, sampleF.g)\n"
		");\n"
	
	"return dw4;\n"
"}\n"

"uint4 Generate_A(float2 cacheCoord)\n" // B8
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(8,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"
	
	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(4,0));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(5,0));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(6,0));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(7,0));\n"
	"float4 sample8 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample9 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sampleA = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sampleB = SampleEFB(subBlockUL+float2(3,1));\n"
	"float4 sampleC = SampleEFB(subBlockUL+float2(4,1));\n"
	"float4 sampleD = SampleEFB(subBlockUL+float2(5,1));\n"
	"float4 sampleE = SampleEFB(subBlockUL+float2(6,1));\n"
	"float4 sampleF = SampleEFB(subBlockUL+float2(7,1));\n"

	"uint4 dw4 = UINT4_8888_BE(\n"
		"255*float4(sample0.b, sample4.b, sample8.b, sampleC.b),\n"
		"255*float4(sample1.b, sample5.b, sample9.b, sampleD.b),\n"
		"255*float4(sample2.b, sample6.b, sampleA.b, sampleE.b),\n"
		"255*float4(sample3.b, sample7.b, sampleB.b, sampleF.b)\n"
		");\n"
	
	"return dw4;\n"
"}\n"

"uint4 Generate_B(float2 cacheCoord)\n" // G8 R8
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(4,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"

	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(3,1));\n"

	"uint4 dw4 = UINT4_8888_BE(\n"
		"255*float4(sample0.g, sample2.g, sample4.g, sample6.g),\n"
		"255*float4(sample0.r, sample2.r, sample4.r, sample6.r),\n"
		"255*float4(sample1.g, sample3.g, sample5.g, sample7.g),\n"
		"255*float4(sample1.r, sample3.r, sample5.r, sample7.r)\n"
		");\n"
	
	"return dw4;\n"
"}\n"

// FIXME: Untested
"uint4 Generate_C(float2 cacheCoord)\n" // B8 G8
"{\n"
	"float2 blockCoord = floor(cacheCoord / float2(2,1));\n"

	"float2 blockUL = blockCoord * float2(4,4);\n"
	"float2 subBlockUL = blockUL + float2(0, 2*(cacheCoord.x%2));\n"

	"float4 sample0 = SampleEFB(subBlockUL+float2(0,0));\n"
	"float4 sample1 = SampleEFB(subBlockUL+float2(1,0));\n"
	"float4 sample2 = SampleEFB(subBlockUL+float2(2,0));\n"
	"float4 sample3 = SampleEFB(subBlockUL+float2(3,0));\n"
	"float4 sample4 = SampleEFB(subBlockUL+float2(0,1));\n"
	"float4 sample5 = SampleEFB(subBlockUL+float2(1,1));\n"
	"float4 sample6 = SampleEFB(subBlockUL+float2(2,1));\n"
	"float4 sample7 = SampleEFB(subBlockUL+float2(3,1));\n"

	"uint4 dw4 = UINT4_8888_BE(\n"
		"255*float4(sample0.b, sample2.b, sample4.b, sample6.b),\n"
		"255*float4(sample0.g, sample2.g, sample4.g, sample6.g),\n"
		"255*float4(sample1.b, sample3.b, sample5.b, sample7.b),\n"
		"255*float4(sample1.g, sample3.g, sample5.g, sample7.g)\n"
		");\n"
	
	"return dw4;\n"
"}\n"

"#ifdef DYNAMIC_MODE\n"
"interface iGenerator\n"
"{\n"
	"uint4 Generate(float2 cacheCoord);\n"
"};\n"

"class cGenerator_4 : iGenerator\n"
"{\n"
	"uint4 Generate(float2 cacheCoord)\n"
	"{ return Generate_4(cacheCoord); }\n"
"};\n"

"class cGenerator_5 : iGenerator\n"
"{\n"
	"uint4 Generate(float2 cacheCoord)\n"
	"{ return Generate_5(cacheCoord); }\n"
"};\n"

"class cGenerator_6 : iGenerator\n"
"{\n"
	"uint4 Generate(float2 cacheCoord)\n"
	"{ return Generate_6(cacheCoord); }\n"
"};\n"

"class cGenerator_8 : iGenerator\n"
"{\n"
	"uint4 Generate(float2 cacheCoord)\n"
	"{ return Generate_8(cacheCoord); }\n"
"};\n"

"class cGenerator_B : iGenerator\n"
"{\n"
	"uint4 Generate(float2 cacheCoord)\n"
	"{ return Generate_B(cacheCoord); }\n"
"};\n"

// Declare generator interface; must be set by application
"iGenerator g_generator;\n"
"#define IMP_GENERATOR g_generator.Generate\n"

"#endif\n"

"#ifndef IMP_GENERATOR\n"
"#error No generator specified\n"
"#endif\n"

"void main(out uint4 ocol0 : SV_Target, in float4 Pos : SV_Position, in float2 fCacheCoord : ENCODECOORD)\n"
"{\n"
	"float2 cacheCoord = floor(fCacheCoord);\n"
	"ocol0 = IMP_GENERATOR(cacheCoord);\n"
"}\n"
;

static const D3D11_INPUT_ELEMENT_DESC QUAD_LAYOUT_DESC[] = {
	{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
};

static const struct QuadVertex
{
	float posX;
	float posY;
} QUAD_VERTS[4] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };

PSTextureEncoder::PSTextureEncoder()
	: m_ready(false), m_outRTV(NULL),
	m_efbEncodeDepthState(NULL),
	m_efbEncodeRastState(NULL), m_efbSampler(NULL),
	m_classLinkage(NULL)
{
	m_ready = false;

	for (size_t i = 0; i < 4; ++i)
		m_fetchClass[i] = NULL;
	for (size_t i = 0; i < 2; ++i)
		m_scaledFetchClass[i] = NULL;
	for (size_t i = 0; i < 2; ++i)
		m_intensityClass[i] = NULL;
	for (size_t i = 0; i < 16; ++i)
		m_generatorClass[i] = NULL;

	// Create output texture RGBA format

	// This format allows us to generate one cache line in two pixels.
	D3D11_TEXTURE2D_DESC t2dd = CD3D11_TEXTURE2D_DESC(
		DXGI_FORMAT_R32G32B32A32_UINT,
		EFB_WIDTH, EFB_HEIGHT/4, 1, 1, D3D11_BIND_RENDER_TARGET);
	m_out = CreateTexture2DShared(&t2dd, NULL);
	CHECK(m_out, "create efb encode output texture");
	D3D::SetDebugObjectName(m_out, "efb encoder output texture");

	// Create output render target view

	D3D11_RENDER_TARGET_VIEW_DESC rtvd = CD3D11_RENDER_TARGET_VIEW_DESC(m_out,
		D3D11_RTV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32G32B32A32_UINT);
	HRESULT hr = D3D::g_device->CreateRenderTargetView(m_out, &rtvd, &m_outRTV);
	CHECK(SUCCEEDED(hr), "create efb encode output render target view");
	D3D::SetDebugObjectName(m_outRTV, "efb encoder output rtv");

	// Create output staging buffer
	
	t2dd.Usage = D3D11_USAGE_STAGING;
	t2dd.BindFlags = 0;
	t2dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	m_outStage = CreateTexture2DShared(&t2dd, NULL);
	CHECK(m_outStage, "create efb encode output staging buffer");
	D3D::SetDebugObjectName(m_outStage, "efb encoder output staging buffer");

	// Create constant buffer for uploading data to shaders

	D3D11_BUFFER_DESC bd = CD3D11_BUFFER_DESC(sizeof(EFBEncodeParams),
		D3D11_BIND_CONSTANT_BUFFER);
	m_encodeParams = CreateBufferShared(&bd, NULL);
	CHECK(m_encodeParams, "create efb encode params buffer");
	D3D::SetDebugObjectName(m_encodeParams, "efb encoder params buffer");

	// Create vertex quad

	bd = CD3D11_BUFFER_DESC(sizeof(QUAD_VERTS), D3D11_BIND_VERTEX_BUFFER,
		D3D11_USAGE_IMMUTABLE);
	D3D11_SUBRESOURCE_DATA srd = { QUAD_VERTS, 0, 0 };

	m_quad = CreateBufferShared(&bd, &srd);
	CHECK(m_quad, "create efb encode quad vertex buffer");
	D3D::SetDebugObjectName(m_quad, "efb encoder quad vertex buffer");

	// Create vertex shader
	SharedPtr<ID3D10Blob> bytecode;
	m_vShader = D3D::CompileAndCreateVertexShader(EFB_ENCODE_VS, sizeof(EFB_ENCODE_VS), std::addressof(bytecode));
	CHECK(m_vShader, "compile/create efb encode vertex shader");
	D3D::SetDebugObjectName(m_vShader, "efb encoder vertex shader");

	// Create input layout for vertex quad using bytecode from vertex shader
	m_quadLayout = CreateInputLayoutShared(QUAD_LAYOUT_DESC,
		sizeof(QUAD_LAYOUT_DESC) / sizeof(D3D11_INPUT_ELEMENT_DESC),
		bytecode->GetBufferPointer(), bytecode->GetBufferSize());
	CHECK(m_quadLayout, "create efb encode quad vertex layout");
	D3D::SetDebugObjectName(m_quadLayout, "efb encoder quad layout");

	// Create pixel shader

#ifdef USE_DYNAMIC_MODE
	if (!InitDynamicMode())
#else
	if (!InitStaticMode())
#endif
		return;

	// Create blend state
	{
	auto const bld = CD3D11_BLEND_DESC(CD3D11_DEFAULT());
	m_efbEncodeBlendState = CreateBlendStateShared(&bld);
	CHECK(SUCCEEDED(hr), "create efb encode blend state");
	D3D::SetDebugObjectName(m_efbEncodeBlendState, "efb encoder blend state");
	}

	// Create depth state
	{
	auto dsd = CD3D11_DEPTH_STENCIL_DESC(CD3D11_DEFAULT());
	dsd.DepthEnable = FALSE;
	hr = D3D::g_device->CreateDepthStencilState(&dsd, &m_efbEncodeDepthState);
	CHECK(SUCCEEDED(hr), "create efb encode depth state");
	D3D::SetDebugObjectName(m_efbEncodeDepthState, "efb encoder depth state");
	}

	// Create rasterizer state
	{
	auto rd = CD3D11_RASTERIZER_DESC(CD3D11_DEFAULT());
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = FALSE;
	hr = D3D::g_device->CreateRasterizerState(&rd, &m_efbEncodeRastState);
	CHECK(SUCCEEDED(hr), "create efb encode rast state");
	D3D::SetDebugObjectName(m_efbEncodeRastState, "efb encoder rast state");
	}

	// Create efb texture sampler
	{
	auto sd = CD3D11_SAMPLER_DESC(CD3D11_DEFAULT());
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	hr = D3D::g_device->CreateSamplerState(&sd, &m_efbSampler);
	CHECK(SUCCEEDED(hr), "create efb encode texture sampler");
	D3D::SetDebugObjectName(m_efbSampler, "efb encoder texture sampler");
	}

	m_ready = true;
}

PSTextureEncoder::~PSTextureEncoder()
{
	for (size_t i = 0; i < 4; ++i)
		SAFE_RELEASE(m_fetchClass[i]);
	for (size_t i = 0; i < 2; ++i)
		SAFE_RELEASE(m_scaledFetchClass[i]);
	for (size_t i = 0; i < 2; ++i)
		SAFE_RELEASE(m_intensityClass[i]);
	for (size_t i = 0; i < 16; ++i)
		SAFE_RELEASE(m_generatorClass[i]);
	
	SAFE_RELEASE(m_classLinkage);

	SAFE_RELEASE(m_efbSampler);
	SAFE_RELEASE(m_efbEncodeRastState);
	SAFE_RELEASE(m_efbEncodeDepthState);
	SAFE_RELEASE(m_outRTV);
}

size_t PSTextureEncoder::Encode(u8* dst, unsigned int dstFormat,
	unsigned int srcFormat, const EFBRectangle& srcRect, bool isIntensity,
	bool scaleByHalf)
{
	if (!m_ready) // Make sure we initialized OK
		return 0;

	// Clamp srcRect to 640x528. BPS: The Strike tries to encode an 800x600
	// texture, which is invalid.
	EFBRectangle correctSrc = srcRect;
	correctSrc.ClampUL(0, 0, EFB_WIDTH, EFB_HEIGHT);

	// Validate source rect size
	if (correctSrc.GetWidth() <= 0 || correctSrc.GetHeight() <= 0)
		return 0;

	HRESULT hr;

	unsigned int blockW = BLOCK_WIDTHS[dstFormat];
	unsigned int blockH = BLOCK_HEIGHTS[dstFormat];

	// Round up source dims to multiple of block size
	unsigned int actualWidth = correctSrc.GetWidth() / (scaleByHalf ? 2 : 1);
	actualWidth = (actualWidth + blockW-1) & ~(blockW-1);
	unsigned int actualHeight = correctSrc.GetHeight() / (scaleByHalf ? 2 : 1);
	actualHeight = (actualHeight + blockH-1) & ~(blockH-1);

	unsigned int numBlocksX = actualWidth/blockW;
	unsigned int numBlocksY = actualHeight/blockH;

	unsigned int cacheLinesPerRow;
	if (dstFormat == 0x6) // RGBA takes two cache lines per block; all others take one
		cacheLinesPerRow = numBlocksX*2;
	else
		cacheLinesPerRow = numBlocksX;
	_assert_msg_(VIDEO, cacheLinesPerRow*32 <= MAX_BYTES_PER_BLOCK_ROW, "cache lines per row sanity check");

	unsigned int totalCacheLines = cacheLinesPerRow * numBlocksY;
	_assert_msg_(VIDEO, totalCacheLines*32 <= MAX_BYTES_PER_ENCODE, "total encode size sanity check");

	size_t encodeSize = 0;
	
	// Reset API

	g_renderer->ResetAPIState();

	// Set up all the state for EFB encoding
	
#ifdef USE_DYNAMIC_MODE
	if (SetDynamicShader(dstFormat, srcFormat, isIntensity, scaleByHalf))
#else
	if (SetStaticShader(dstFormat, srcFormat, isIntensity, scaleByHalf))
#endif
	{
		D3D::g_context->VSSetShader(m_vShader, NULL, 0);

		D3D::stateman->PushBlendState(m_efbEncodeBlendState);
		D3D::stateman->PushDepthState(m_efbEncodeDepthState);
		D3D::stateman->PushRasterizerState(m_efbEncodeRastState);
		D3D::stateman->Apply();
	
		D3D11_VIEWPORT vp = CD3D11_VIEWPORT(0.f, 0.f, FLOAT(cacheLinesPerRow*2), FLOAT(numBlocksY));
		D3D::g_context->RSSetViewports(1, &vp);

		D3D::g_context->IASetInputLayout(m_quadLayout);
		D3D::g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		UINT stride = sizeof(QuadVertex);
		UINT offset = 0;
		D3D::g_context->IASetVertexBuffers(0, 1, &m_quad, &stride, &offset);
	
		EFBRectangle fullSrcRect;
		fullSrcRect.left = 0;
		fullSrcRect.top = 0;
		fullSrcRect.right = EFB_WIDTH;
		fullSrcRect.bottom = EFB_HEIGHT;
		TargetRectangle targetRect = g_renderer->ConvertEFBRectangle(fullSrcRect);
	
		EFBEncodeParams params = { 0 };
		params.NumHalfCacheLinesX = FLOAT(cacheLinesPerRow*2);
		params.NumBlocksY = FLOAT(numBlocksY);
		params.PosX = FLOAT(correctSrc.left);
		params.PosY = FLOAT(correctSrc.top);
		params.TexLeft = float(targetRect.left) / g_renderer->GetFullTargetWidth();
		params.TexTop = float(targetRect.top) / g_renderer->GetFullTargetHeight();
		params.TexRight = float(targetRect.right) / g_renderer->GetFullTargetWidth();
		params.TexBottom = float(targetRect.bottom) / g_renderer->GetFullTargetHeight();
		D3D::g_context->UpdateSubresource(m_encodeParams, 0, NULL, &params, 0, 0);

		D3D::g_context->VSSetConstantBuffers(0, 1, &m_encodeParams);
	
		D3D::g_context->OMSetRenderTargets(1, &m_outRTV, NULL);

		ID3D11ShaderResourceView* pEFB = (srcFormat == PIXELFMT_Z24) ?
			FramebufferManager::GetEFBDepthTexture()->GetSRV() :
			FramebufferManager::GetEFBColorTexture()->GetSRV();

		D3D::g_context->PSSetConstantBuffers(0, 1, &m_encodeParams);
		D3D::g_context->PSSetShaderResources(0, 1, &pEFB);
		D3D::g_context->PSSetSamplers(0, 1, &m_efbSampler);

		// Encode!

		D3D::g_context->Draw(4, 0);

		// Copy to staging buffer

		D3D11_BOX srcBox = CD3D11_BOX(0, 0, 0, cacheLinesPerRow*2, numBlocksY, 1);
		D3D::g_context->CopySubresourceRegion(m_outStage, 0, 0, 0, 0, m_out, 0, &srcBox);

		// Clean up state
	
		IUnknown* nullDummy = NULL;

		D3D::g_context->PSSetSamplers(0, 1, (ID3D11SamplerState**)&nullDummy);
		D3D::g_context->PSSetShaderResources(0, 1, (ID3D11ShaderResourceView**)&nullDummy);
		D3D::g_context->PSSetConstantBuffers(0, 1, (ID3D11Buffer**)&nullDummy);
	
		D3D::g_context->OMSetRenderTargets(0, NULL, NULL);

		D3D::g_context->VSSetConstantBuffers(0, 1, (ID3D11Buffer**)&nullDummy);
		
		D3D::stateman->PopRasterizerState();
		D3D::stateman->PopDepthState();
		D3D::stateman->PopBlendState();

		D3D::g_context->PSSetShader(NULL, NULL, 0);
		D3D::g_context->VSSetShader(NULL, NULL, 0);

		// Transfer staging buffer to GameCube/Wii RAM

		D3D11_MAPPED_SUBRESOURCE map = { 0 };
		hr = D3D::g_context->Map(m_outStage, 0, D3D11_MAP_READ, 0, &map);
		CHECK(SUCCEEDED(hr), "map staging buffer");

		u8* src = (u8*)map.pData;
		for (unsigned int y = 0; y < numBlocksY; ++y)
		{
			memcpy(dst, src, cacheLinesPerRow*32);
			dst += bpmem.copyMipMapStrideChannels*32;
			src += map.RowPitch;
		}

		D3D::g_context->Unmap(m_outStage, 0);

		encodeSize = bpmem.copyMipMapStrideChannels*32 * numBlocksY;
	}

	// Restore API

	g_renderer->RestoreAPIState();
	D3D::g_context->OMSetRenderTargets(1, &FramebufferManager::GetEFBColorTexture()->GetRTV(),
		FramebufferManager::GetEFBDepthTexture()->GetDSV());

	return encodeSize;
}

bool PSTextureEncoder::InitStaticMode()
{
	// Nothing to really do.
	return true;
}

static const char* FETCH_FUNC_NAMES[4] = {
	"Fetch_0", "Fetch_1", "Fetch_2", "Fetch_3"
};

static const char* SCALEDFETCH_FUNC_NAMES[2] = {
	"ScaledFetch_0", "ScaledFetch_1"
};

static const char* INTENSITY_FUNC_NAMES[2] = {
	"Intensity_0", "Intensity_1"
};

bool PSTextureEncoder::SetStaticShader(unsigned int dstFormat, unsigned int srcFormat,
	bool isIntensity, bool scaleByHalf)
{
	size_t fetchNum = srcFormat;
	size_t scaledFetchNum = scaleByHalf ? 1 : 0;
	size_t intensityNum = isIntensity ? 1 : 0;
	size_t generatorNum = dstFormat;

	ComboKey key = MakeComboKey(dstFormat, srcFormat, isIntensity, scaleByHalf);

	ComboMap::iterator it = m_staticShaders.find(key);
	if (it == m_staticShaders.end())
	{
		const char* generatorFuncName = NULL;
		switch (generatorNum)
		{
		case 0x0: generatorFuncName = "Generate_0"; break;
		case 0x1: generatorFuncName = "Generate_1"; break;
		case 0x2: generatorFuncName = "Generate_2"; break;
		case 0x3: generatorFuncName = "Generate_3"; break;
		case 0x4: generatorFuncName = "Generate_4"; break;
		case 0x5: generatorFuncName = "Generate_5"; break;
		case 0x6: generatorFuncName = "Generate_6"; break;
		case 0x7: generatorFuncName = "Generate_7"; break;
		case 0x8: generatorFuncName = "Generate_8"; break;
		case 0x9: generatorFuncName = "Generate_9"; break;
		case 0xA: generatorFuncName = "Generate_A"; break;
		case 0xB: generatorFuncName = "Generate_B"; break;
		case 0xC: generatorFuncName = "Generate_C"; break;
		default:
			WARN_LOG(VIDEO, "No generator available for dst format 0x%X; aborting", generatorNum);
			m_staticShaders[key].reset();
			return false;
			break;
		}

		INFO_LOG(VIDEO, "Compiling efb encoding shader for dstFormat 0x%X, srcFormat %d, isIntensity %d, scaleByHalf %d",
			dstFormat, srcFormat, isIntensity ? 1 : 0, scaleByHalf ? 1 : 0);

		// Shader permutation not found, so compile it
		D3D_SHADER_MACRO macros[] = {
			{ "IMP_FETCH", FETCH_FUNC_NAMES[fetchNum] },
			{ "IMP_SCALEDFETCH", SCALEDFETCH_FUNC_NAMES[scaledFetchNum] },
			{ "IMP_INTENSITY", INTENSITY_FUNC_NAMES[intensityNum] },
			{ "IMP_GENERATOR", generatorFuncName },
			{ NULL, NULL }
		};

		auto const bytecode = D3D::CompilePixelShader(EFB_ENCODE_PS, sizeof(EFB_ENCODE_PS), macros);
		if (!bytecode)
		{
			WARN_LOG(VIDEO, "EFB encoder shader for dstFormat 0x%X, srcFormat %d, isIntensity %d, scaleByHalf %d failed to compile",
				dstFormat, srcFormat, isIntensity ? 1 : 0, scaleByHalf ? 1 : 0);
			// Add dummy shader to map to prevent trying to compile over and
			// over again
			m_staticShaders[key].reset();
			return false;
		}

		ID3D11PixelShader* newShader = nullptr;
		HRESULT hr = D3D::g_device->CreatePixelShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), NULL, &newShader);
		CHECK(SUCCEEDED(hr), "create efb encoder pixel shader");

		it = m_staticShaders.insert(std::make_pair(key, SharedPtr<ID3D11PixelShader>::FromPtr(newShader))).first;
	}

	if (it != m_staticShaders.end())
	{
		if (it->second)
		{
			D3D::g_context->PSSetShader(it->second, NULL, 0);
			return true;
		}
		else
			return false;
	}
	else
		return false;
}

bool PSTextureEncoder::InitDynamicMode()
{
	const D3D_SHADER_MACRO macros[] = {
		{ "DYNAMIC_MODE", NULL },
		{ NULL, NULL }
	};

	HRESULT hr = D3D::g_device->CreateClassLinkage(&m_classLinkage);
	CHECK(SUCCEEDED(hr), "create efb encode class linkage");
	D3D::SetDebugObjectName(m_classLinkage, "efb encoder class linkage");

	SharedPtr<ID3D10Blob> bytecode;
	m_dynamicShader = D3D::CompileAndCreatePixelShader(EFB_ENCODE_PS, sizeof(EFB_ENCODE_PS), macros, std::addressof(bytecode));
	CHECK(m_dynamicShader, "compile/create efb encode pixel shader");
	D3D::SetDebugObjectName(m_dynamicShader, "efb encoder pixel shader");
	
	// Use D3DReflect

	ID3D11ShaderReflection* reflect = NULL;
	hr = PD3DReflect(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), IID_ID3D11ShaderReflection, (void**)&reflect);
	CHECK(SUCCEEDED(hr), "reflect on efb encoder shader");

	// Get number of slots and create dynamic linkage array

	UINT numSlots = reflect->GetNumInterfaceSlots();
	m_linkageArray.resize(numSlots, NULL);

	// Get interface slots

	ID3D11ShaderReflectionVariable* var = reflect->GetVariableByName("g_fetch");
	m_fetchSlot = var->GetInterfaceSlot(0);

	var = reflect->GetVariableByName("g_scaledFetch");
	m_scaledFetchSlot = var->GetInterfaceSlot(0);

	var = reflect->GetVariableByName("g_intensity");
	m_intensitySlot = var->GetInterfaceSlot(0);

	var = reflect->GetVariableByName("g_generator");
	m_generatorSlot = var->GetInterfaceSlot(0);

	INFO_LOG(VIDEO, "fetch slot %d, scaledFetch slot %d, intensity slot %d, generator slot %d",
		m_fetchSlot, m_scaledFetchSlot, m_intensitySlot, m_generatorSlot);

	// Class instances will be created at the time they are used

	for (size_t i = 0; i < 4; ++i)
		m_fetchClass[i] = NULL;
	for (size_t i = 0; i < 2; ++i)
		m_scaledFetchClass[i] = NULL;
	for (size_t i = 0; i < 2; ++i)
		m_intensityClass[i] = NULL;
	for (size_t i = 0; i < 16; ++i)
		m_generatorClass[i] = NULL;

	reflect->Release();

	return true;
}

static const char* FETCH_CLASS_NAMES[4] = {
	"cFetch_0", "cFetch_1", "cFetch_2", "cFetch_3"
};

static const char* SCALEDFETCH_CLASS_NAMES[2] = {
	"cScaledFetch_0", "cScaledFetch_1"
};

static const char* INTENSITY_CLASS_NAMES[2] = {
	"cIntensity_0", "cIntensity_1"
};

bool PSTextureEncoder::SetDynamicShader(unsigned int dstFormat,
	unsigned int srcFormat, bool isIntensity, bool scaleByHalf)
{
	size_t fetchNum = srcFormat;
	size_t scaledFetchNum = scaleByHalf ? 1 : 0;
	size_t intensityNum = isIntensity ? 1 : 0;
	size_t generatorNum = dstFormat;

	// FIXME: Not all the possible generators are available as classes yet.
	// When dynamic mode is usable, implement them.
	const char* generatorName = NULL;
	switch (generatorNum)
	{
	case 0x4: generatorName = "cGenerator_4"; break;
	case 0x5: generatorName = "cGenerator_5"; break;
	case 0x6: generatorName = "cGenerator_6"; break;
	case 0x8: generatorName = "cGenerator_8"; break;
	case 0xB: generatorName = "cGenerator_B"; break;
	default:
		WARN_LOG(VIDEO, "No generator available for dst format 0x%X; aborting", generatorNum);
		return false;
		break;
	}

	// Make sure class instances are available
	if (!m_fetchClass[fetchNum])
	{
		INFO_LOG(VIDEO, "Creating %s class instance for encoder 0x%X",
			FETCH_CLASS_NAMES[fetchNum], dstFormat);
		HRESULT hr = m_classLinkage->CreateClassInstance(
			FETCH_CLASS_NAMES[fetchNum], 0, 0, 0, 0, &m_fetchClass[fetchNum]);
		CHECK(SUCCEEDED(hr), "create fetch class instance");
	}
	if (!m_scaledFetchClass[scaledFetchNum])
	{
		INFO_LOG(VIDEO, "Creating %s class instance for encoder 0x%X",
			SCALEDFETCH_CLASS_NAMES[scaledFetchNum], dstFormat);
		HRESULT hr = m_classLinkage->CreateClassInstance(
			SCALEDFETCH_CLASS_NAMES[scaledFetchNum], 0, 0, 0, 0,
			&m_scaledFetchClass[scaledFetchNum]);
		CHECK(SUCCEEDED(hr), "create scaled fetch class instance");
	}
	if (!m_intensityClass[intensityNum])
	{
		INFO_LOG(VIDEO, "Creating %s class instance for encoder 0x%X",
			INTENSITY_CLASS_NAMES[intensityNum], dstFormat);
		HRESULT hr = m_classLinkage->CreateClassInstance(
			INTENSITY_CLASS_NAMES[intensityNum], 0, 0, 0, 0,
			&m_intensityClass[intensityNum]);
		CHECK(SUCCEEDED(hr), "create intensity class instance");
	}
	if (!m_generatorClass[generatorNum])
	{
		INFO_LOG(VIDEO, "Creating %s class instance for encoder 0x%X",
			generatorName, dstFormat);
		HRESULT hr = m_classLinkage->CreateClassInstance(
			generatorName, 0, 0, 0, 0, &m_generatorClass[generatorNum]);
		CHECK(SUCCEEDED(hr), "create generator class instance");
	}

	// Assemble dynamic linkage array
	if (m_fetchSlot != UINT(-1))
		m_linkageArray[m_fetchSlot] = m_fetchClass[fetchNum];
	if (m_scaledFetchSlot != UINT(-1))
		m_linkageArray[m_scaledFetchSlot] = m_scaledFetchClass[scaledFetchNum];
	if (m_intensitySlot != UINT(-1))
		m_linkageArray[m_intensitySlot] = m_intensityClass[intensityNum];
	if (m_generatorSlot != UINT(-1))
		m_linkageArray[m_generatorSlot] = m_generatorClass[generatorNum];
	
	D3D::g_context->PSSetShader(m_dynamicShader,
		m_linkageArray.empty() ? NULL : &m_linkageArray[0],
		(UINT)m_linkageArray.size());

	return true;
}

}
