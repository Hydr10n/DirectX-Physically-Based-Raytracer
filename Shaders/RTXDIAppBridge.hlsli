#pragma once

#define RTXDI_PRESAMPLING_GROUP_SIZE 256
#define RTXDI_SCREEN_SPACE_GROUP_SIZE 8

#define RTXDI_REGIR_MODE RTXDI_REGIR_ONION

#define RTXDI_ENABLE_BOILING_FILTER
#define RTXDI_BOILING_FILTER_GROUP_SIZE RTXDI_SCREEN_SPACE_GROUP_SIZE

#include "Rtxdi/DI/ReSTIRDIParameters.h"
#include "Rtxdi/Utils/Math.hlsli"

#include "Common.hlsli"

#include "Camera.hlsli"

#include "Light.hlsli"

#include "Denoiser.hlsli"

SamplerState g_anisotropicSampler : register(s0);

RaytracingAccelerationStructure g_scene : register(t0);

struct GraphicsSettings
{
	uint2 RenderSize;
	bool IsLastRenderPass, IsReGIRCellVisualizationEnabled;
	struct
	{
		RTXDI_RISBufferSegmentParameters LocalLightRISBufferSegment, EnvironmentLightRISBufferSegment;
		RTXDI_LightBufferParameters LightBuffer;
		RTXDI_RuntimeParameters Runtime;
		ReGIR_Parameters ReGIR;
		ReSTIRDI_Parameters ReSTIRDI;
	} RTXDI;
	Denoiser Denoiser;
	uint3 _;
};
ConstantBuffer<GraphicsSettings> g_graphicsSettings : register(b0);

ConstantBuffer<Camera> g_camera : register(b1);

StructuredBuffer<ObjectData> g_objectData : register(t1);
StructuredBuffer<LightInfo> g_lightInfo : register(t2);
StructuredBuffer<uint> g_lightIndices : register(t3);
Buffer<float2> g_neighborOffsets : register(t4);

Texture2D g_localLightPDF : register(t5);
Texture2D<float2> g_previousGeometricNormal : register(t6);
Texture2D<float2> g_geometricNormal : register(t7);
Texture2D<float> g_previousLinearDepth : register(t8);
Texture2D<float> g_linearDepth : register(t9);
Texture2D<float3> g_motionVector : register(t10);
Texture2D<float4> g_previousBaseColorMetalness : register(t11);
Texture2D<float4> g_baseColorMetalness : register(t12);
Texture2D<float4> g_previousNormalRoughness : register(t13);
Texture2D<float4> g_normalRoughness : register(t14);
Texture2D<float> g_previousIOR : register(t15);
Texture2D<float> g_IOR : register(t16);
Texture2D<float> g_previousTransmission : register(t17);
Texture2D<float> g_transmission : register(t18);

RWStructuredBuffer<uint2> g_RIS : register(u0);
RWStructuredBuffer<RTXDI_PackedDIReservoir> g_DIReservoir : register(u1);

RWTexture2D<float3> g_radiance : register(u2);
RWTexture2D<float4> g_diffuse : register(u3);
RWTexture2D<float4> g_specular : register(u4);
RWTexture2D<float> g_specularHitDistance : register(u5);

#include "RaytracingHelpers.hlsli"

#define RTXDI_NEIGHBOR_OFFSETS_BUFFER g_neighborOffsets
#define RTXDI_RIS_BUFFER g_RIS
#define RTXDI_LIGHT_RESERVOIR_BUFFER g_DIReservoir

#define ROOT_SIGNATURE \
	[RootSignature( \
		"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
		"StaticSampler(s0)," \
		"SRV(t0)," \
		"CBV(b0)," \
		"CBV(b1)," \
		"SRV(t1)," \
		"SRV(t2)," \
		"SRV(t3)," \
		"DescriptorTable(SRV(t4))," \
		"DescriptorTable(SRV(t5))," \
		"DescriptorTable(SRV(t6))," \
		"DescriptorTable(SRV(t7))," \
		"DescriptorTable(SRV(t8))," \
		"DescriptorTable(SRV(t9))," \
		"DescriptorTable(SRV(t10))," \
		"DescriptorTable(SRV(t11))," \
		"DescriptorTable(SRV(t12))," \
		"DescriptorTable(SRV(t13))," \
		"DescriptorTable(SRV(t14))," \
		"DescriptorTable(SRV(t15))," \
		"DescriptorTable(SRV(t16))," \
		"DescriptorTable(SRV(t17))," \
		"DescriptorTable(SRV(t18))," \
		"UAV(u0)," \
		"UAV(u1)," \
		"DescriptorTable(UAV(u2))," \
		"DescriptorTable(UAV(u3))," \
		"DescriptorTable(UAV(u4))," \
		"DescriptorTable(UAV(u5))" \
	)]

int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame)
{
	// Reflect the position across the screen edges.
	// Compared to simple clamping, this prevents the spread of colorful blobs from screen edges.
	const int2 pixelDimensions = (int2)g_graphicsSettings.RenderSize;
	if (pixelPosition.x < 0)
	{
		pixelPosition.x = -pixelPosition.x;
	}
	if (pixelPosition.y < 0)
	{
		pixelPosition.y = -pixelPosition.y;
	}
	if (pixelPosition.x >= pixelDimensions.x)
	{
		pixelPosition.x = 2 * pixelDimensions.x - pixelPosition.x - 1;
	}
	if (pixelPosition.y >= pixelDimensions.y)
	{
		pixelPosition.y = 2 * pixelDimensions.y - pixelPosition.y - 1;
	}
	return pixelPosition;
}

struct RAB_RandomSamplerState
{
	uint Seed, Index;

	void Initialize(uint2 pixelPosition, uint frameIndex)
	{
		Index = 1;
		Seed = RTXDI_JenkinsHash(RTXDI_ZCurveToLinearIndex(pixelPosition)) + frameIndex;
	}

	float GetFloat()
	{
#define ROT32(x, y) ((x << y) | (x >> (32 - y)))
		// https://en.wikipedia.org/wiki/MurmurHash
		const uint c1 = 0xcc9e2d51, c2 = 0x1b873593, r1 = 15, r2 = 13, m = 5, n = 0xe6546b64;
		uint hash = Seed, k = Index++;
		k *= c1;
		k = ROT32(k, r1);
		k *= c2;
		hash ^= k;
		hash = ROT32(hash, r2) * m + n;
		hash ^= 4;
		hash ^= hash >> 16;
		hash *= 0x85ebca6b;
		hash ^= hash >> 13;
		hash *= 0xc2b2ae35;
		hash ^= hash >> 16;
#undef ROT32
		const uint one = asuint(1.0f), mask = (1 << 23) - 1;
		return asfloat((mask & hash) | one) - 1;
	}

	float2 GetFloat2()
	{
		return float2(GetFloat(), GetFloat());
	}

	float3 GetFloat3()
	{
		return float3(GetFloat2(), GetFloat());
	}

	float4 GetFloat4()
	{
		return float4(GetFloat3(), GetFloat());
	}
};

RAB_RandomSamplerState RAB_InitRandomSampler(uint2 pixelPos, uint frameIndex)
{
	RAB_RandomSamplerState state;
	state.Initialize(pixelPos, frameIndex);
	return state;
}

float RAB_GetNextRandom(inout RAB_RandomSamplerState rng)
{
	return rng.GetFloat();
}

int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
	return int(lightIndex);
}

using RAB_LightInfo = LightInfo;

RAB_LightInfo RAB_EmptyLightInfo()
{
	return (RAB_LightInfo)0;
}

RAB_LightInfo RAB_LoadLightInfo(uint index, bool previousFrame)
{
	return g_lightInfo[index];
}

RAB_LightInfo RAB_LoadCompactLightInfo(uint linearIndex)
{
	return RAB_EmptyLightInfo();
}

bool RAB_StoreCompactLightInfo(uint linearIndex, RAB_LightInfo lightInfo)
{
	return false;
}

using RAB_LightSample = LightSample;

RAB_LightSample RAB_EmptyLightSample()
{
	return (RAB_LightSample)0;
}

bool RAB_IsAnalyticLightSample(RAB_LightSample lightSample)
{
	return false;
}

float RAB_LightSampleSolidAnglePdf(RAB_LightSample lightSample)
{
	return lightSample.SolidAnglePDF;
}

using RAB_Material = BSDFSample;

struct RAB_Surface
{
	float LinearDepth;
	float3 Position, ViewDirection;
	SurfaceVectors Vectors;
	RAB_Material Material;
	LobeWeightArray LobeWeights;

	bool Sample(inout RAB_RandomSamplerState rng, out float3 L)
	{
		LobeType lobeType;
		return Material.Sample(Vectors, ViewDirection, LobeWeights, rng.GetFloat4(), L, lobeType);
	}

	float EvaluatePDF(float3 L)
	{
		return Material.EvaluatePDF(Vectors, L, ViewDirection, LobeWeights);
	}

	void Evaluate(float3 L, out float3 diffuse, out float3 specular)
	{
		Material.Evaluate(Vectors, L, ViewDirection, LobeWeights, diffuse, specular);
	}

	void Shade(float3 samplePosition, float3 radiance, out float3 diffuse, out float3 specular)
	{
		Evaluate(normalize(samplePosition - Position), diffuse, specular);
		diffuse *= radiance;
		specular *= radiance;
	}

	void Shade(RAB_LightSample lightSample, out float3 diffuse, out float3 specular)
	{
		diffuse = specular = 0;
		if (lightSample.SolidAnglePDF > 0)
		{
			Shade(lightSample.Position, lightSample.Radiance, diffuse, specular);
			const float invPDF = 1 / lightSample.SolidAnglePDF;
			diffuse *= invPDF;
			specular *= invPDF;
		}
	}
};

RAB_Surface RAB_EmptySurface()
{
	RAB_Surface surface = (RAB_Surface)0;
	surface.LinearDepth = 1.#INF;
	return surface;
}

RAB_Surface RAB_GetGBufferSurface(int2 pixelPosition, bool previousFrame)
{
	const float MinRoughness = 0.05f;
	RAB_Surface surface = RAB_EmptySurface();
	if (all(pixelPosition < g_graphicsSettings.RenderSize))
	{
		float2 geometricNormal;
		float4 baseColorMetalness, normalRoughenss;
		float IOR, transmission = 0;
		if (previousFrame)
		{
			if (!isfinite(surface.LinearDepth = g_previousLinearDepth[pixelPosition])
				|| (normalRoughenss = g_previousNormalRoughness[pixelPosition]).w < MinRoughness)
			{
				return RAB_EmptySurface();
			}
			geometricNormal = g_previousGeometricNormal[pixelPosition];
			baseColorMetalness = g_previousBaseColorMetalness[pixelPosition];
			IOR = g_previousIOR[pixelPosition];
			if (baseColorMetalness.a < 1)
			{
				transmission = g_previousTransmission[pixelPosition];
			}
		}
		else
		{
			if (!isfinite(surface.LinearDepth = g_linearDepth[pixelPosition])
				|| (normalRoughenss = g_normalRoughness[pixelPosition]).w < MinRoughness)
			{
				return RAB_EmptySurface();
			}
			geometricNormal = g_geometricNormal[pixelPosition];
			baseColorMetalness = g_baseColorMetalness[pixelPosition];
			IOR = g_IOR[pixelPosition];
			if (baseColorMetalness.a < 1)
			{
				transmission = g_transmission[pixelPosition];
			}
		}
		surface.Position = g_camera.ReconstructWorldPosition(Math::CalculateNDC(Math::CalculateUV(pixelPosition, g_graphicsSettings.RenderSize, g_camera.Jitter)), surface.LinearDepth, previousFrame);
		surface.ViewDirection = normalize((previousFrame ? g_camera.PreviousPosition : g_camera.Position) - surface.Position);
		const float3 normal = Packing::DecodeUnitVector(geometricNormal, true);
		const bool isFrontFace = dot(normal, surface.ViewDirection) > 0;
		surface.Vectors.Initialize(isFrontFace, normal, normalRoughenss.xyz);
		surface.Material.Initialize(
			baseColorMetalness.rgb,
			baseColorMetalness.a,
			normalRoughenss.w,
			IOR,
			transmission,
			isFrontFace
		);
		surface.Material.ComputeLobeWeights(surface.Vectors, surface.ViewDirection, surface.LobeWeights);
	}
	return surface;
}

bool RAB_IsSurfaceValid(RAB_Surface surface)
{
	return isfinite(surface.LinearDepth);
}

float3 RAB_GetSurfaceWorldPos(RAB_Surface surface)
{
	return surface.Position;
}

float3 RAB_GetSurfaceNormal(RAB_Surface surface)
{
	return surface.Vectors.ShadingNormal;
}

float3 RAB_GetSurfaceViewDir(RAB_Surface surface)
{
	return surface.ViewDirection;
}

float RAB_GetSurfaceLinearDepth(RAB_Surface surface)
{
	return surface.LinearDepth;
}

RAB_Material RAB_GetMaterial(RAB_Surface surface)
{
	return surface.Material;
}

bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b)
{
	return RTXDI_CompareRelativeDifference(a.Roughness, b.Roughness, 0.5f)
		&& abs(Color::Luminance(a.F0) - Color::Luminance(b.F0)) <= 0.25f
		&& abs(Color::Luminance(a.Albedo) - Color::Luminance(b.Albedo)) <= 0.25f;
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample lightSample, out float3 o_lightDir, out float o_lightDistance)
{
	o_lightDir = lightSample.Position - surface.Position;
	o_lightDistance = length(o_lightDir);
	o_lightDir /= o_lightDistance;
}

bool RAB_GetSurfaceBrdfSample(RAB_Surface surface, inout RAB_RandomSamplerState rng, out float3 dir)
{
	return surface.Sample(rng, dir);
}

float RAB_GetSurfaceBrdfPdf(RAB_Surface surface, float3 dir)
{
	return surface.EvaluatePDF(dir);
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample lightSample, RAB_Surface surface)
{
	float3 diffuse, specular;
	surface.Shade(lightSample, diffuse, specular);
	return Color::Luminance(diffuse + specular);
}

float RAB_GetLightTargetPdfForVolume(RAB_LightInfo light, float3 volumeCenter, float volumeRadius)
{
	TriangleLight triangleLight;
	triangleLight.Load(light);
	return triangleLight.CalculateWeightForVolume(volumeCenter, volumeRadius);
}

RayDesc CreateVisibilityRay(RAB_Surface surface, float3 samplePosition, float offset = 1e-3f)
{
	const float3 L = samplePosition - surface.Position;
	const float Llength = length(L);
	const RayDesc rayDesc = { surface.Position, offset, L / Llength, max(0, Llength - offset * 2) };
	return rayDesc;
}

float3 GetFinalVisibility(RAB_Surface surface, float3 samplePosition)
{
	const RayDesc rayDesc = CreateVisibilityRay(surface, samplePosition);
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
	return TraceRay(q, rayDesc, RAY_FLAG_NONE, ~0u);
}

bool GetConservativeVisibility(RAB_Surface surface, float3 samplePosition)
{
	const RayDesc rayDesc = CreateVisibilityRay(surface, samplePosition);
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
	TraceRay(q, rayDesc, RAY_FLAG_NONE, ~0u);
	return q.CommittedStatus() == COMMITTED_NOTHING;
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, float3 samplePosition)
{
	return GetConservativeVisibility(surface, samplePosition);
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample)
{
	return RAB_GetConservativeVisibility(surface, lightSample.Position);
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, float3 samplePosition)
{
	return RAB_GetConservativeVisibility(currentSurface, samplePosition);
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, RAB_LightSample lightSample)
{
	return RAB_GetTemporalConservativeVisibility(currentSurface, previousSurface, lightSample.Position);
}

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo lightInfo, RAB_Surface surface, float2 uv)
{
	TriangleLight triangleLight;
	triangleLight.Load(lightInfo);
	return triangleLight.CalculateSample(surface.Position, uv);
}

bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax, out uint o_lightIndex, out float2 o_randXY)
{
	o_lightIndex = RTXDI_InvalidLightIndex;
	o_randXY = 0;
	const RayDesc rayDesc = { origin, tMin, direction, tMax };
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	TraceRay(q, rayDesc, RAY_FLAG_NONE, ~0u);
	const bool hit = q.CommittedStatus() != COMMITTED_NOTHING;
	if (hit)
	{
		o_lightIndex = g_lightIndices[q.CommittedInstanceID() + q.CommittedGeometryIndex()];
		if (o_lightIndex != RTXDI_InvalidLightIndex)
		{
			o_lightIndex += q.CommittedPrimitiveIndex();
			o_randXY = Math::RandomFromBarycentrics(q.CommittedTriangleBarycentrics());
		}
	}
	return hit;
}

float RAB_EvaluateLocalLightSourcePdf(uint lightIndex)
{
	uint2 textureSize;
	uint mipLevels;
	g_localLightPDF.GetDimensions(0, textureSize.x, textureSize.y, mipLevels);

	// The single texel in the last mip level is effectively the average of all texels in mip 0,
	// padded to a square shape with zeros. So, in case the PDF texture has a 2:1 aspect ratio,
	// that texel's value is only half of the true average of the rectangular input texture.
	// Compensate for that by assuming that the input texture is square.
	const uint value = 1u << (mipLevels - 1);
	const float sum = g_localLightPDF.mips[mipLevels - 1][(uint2)0].x * value * value;
	return g_localLightPDF[RTXDI_LinearIndexToZCurve(lightIndex)].x / sum;
}

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 worldDir)
{
	return 0;
}

float RAB_EvaluateEnvironmentMapSamplingPdf(float3 L)
{
	return 0;
}
