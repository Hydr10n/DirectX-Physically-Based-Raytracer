#pragma once

#include "Raytracing.hlsli"

#include "TriangleLight.hlsli"

StructuredBuffer<RAB_LightInfo> g_lightInfo : register(t7);
StructuredBuffer<uint> g_lightIndices : register(t8);
Buffer<float2> g_neighborOffsets : register(t9);

RWStructuredBuffer<RTXDI_PackedDIReservoir> g_DIReservoir : register(u10);

#define RTXDI_NEIGHBOR_OFFSETS_BUFFER g_neighborOffsets
#define RTXDI_LIGHT_RESERVOIR_BUFFER g_DIReservoir

typedef uint RAB_RandomSamplerState;
RAB_RandomSamplerState RAB_InitRandomSampler(uint2 pixelPosition, uint frameIndex) { return 0; }
float RAB_GetNextRandom(inout RAB_RandomSamplerState rng) { return STL::Rng::Hash::GetFloat(); }

struct RAB_Surface {
	float3 Position, Normal, GeometricNormal;
	float3 Albedo, Rf0;
	float Roughness, DiffuseProbability;
	float3 ViewDirection;
	float LinearDepth;
};

RAB_Surface RAB_EmptySurface() {
	RAB_Surface surface = (RAB_Surface)0;
	surface.LinearDepth = 1.#INFf;
	return surface;
}

RAB_Surface RAB_GetGBufferSurface(int2 pixelPosition, bool previousFrame) {
	RAB_Surface surface = RAB_EmptySurface();
	if (previousFrame
		&& all(pixelPosition < g_graphicsSettings.RenderSize)
		&& (surface.LinearDepth = g_previousLinearDepth[pixelPosition]) != 1.#INFf) {
		surface.Position = g_camera.ReconstructPreviousWorldPosition(Math::CalculateNDC(Math::CalculateUV(pixelPosition, g_graphicsSettings.RenderSize)), surface.LinearDepth);
		const float4 normalRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(g_previousNormalRoughness[pixelPosition]);
		surface.Normal = normalRoughness.xyz;
		surface.GeometricNormal = STL::Packing::DecodeUnitVector(g_previousGeometricNormals[pixelPosition], true);
		surface.Roughness = max(normalRoughness.w, MinRoughness);
		const float4 baseColorMetalness = g_previousBaseColorMetalness[pixelPosition];
		STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(baseColorMetalness.rgb, baseColorMetalness.a, surface.Albedo, surface.Rf0);
		surface.ViewDirection = normalize(g_camera.Position - surface.Position);
		surface.DiffuseProbability = Material::EstimateDiffuseProbability(surface.Albedo, surface.Rf0, surface.Roughness, abs(dot(surface.Normal, surface.ViewDirection)));
	}
	return surface;
}

bool RAB_IsSurfaceValid(RAB_Surface surface) { return surface.LinearDepth != 1.#INFf; }
float3 RAB_GetSurfaceWorldPos(RAB_Surface surface) { return surface.Position; }
float3 RAB_GetSurfaceNormal(RAB_Surface surface) { return surface.Normal; }
float3 RAB_GetSurfaceViewDir(RAB_Surface surface) { return surface.ViewDirection; }
float RAB_GetSurfaceLinearDepth(RAB_Surface surface) { return surface.LinearDepth; }

int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame) { return clamp(pixelPosition, 0, int2(g_graphicsSettings.RenderSize) - 1); }

RAB_LightInfo RAB_LoadLightInfo(uint index, bool previousFrame) { return g_lightInfo[index]; }
int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious) { return int(lightIndex); }

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 worldDir) { return 0; }
float RAB_EvaluateEnvironmentMapSamplingPdf(float3 L) { return 0; }

float RAB_EvaluateLocalLightSourcePdf(uint lightIndex) { return 1.0f / g_graphicsSettings.RTXDI.LightBufferParameters.localLightBufferRegion.numLights; }

bool RAB_GetSurfaceBrdfSample(RAB_Surface surface, inout RAB_RandomSamplerState rng, out float3 dir) {
	const float3x3 basis = STL::Geometry::GetBasis(surface.Normal);
	const float2 randomValue = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
	if (RAB_GetNextRandom(rng) < surface.DiffuseProbability) dir = STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::Cosine::GetRay(randomValue));
	else {
		dir = reflect(-surface.ViewDirection, STL::Geometry::RotateVectorInverse(basis, STL::ImportanceSampling::VNDF::GetRay(randomValue, surface.Roughness, STL::Geometry::RotateVector(basis, surface.ViewDirection))));
	}
	return dot(surface.Normal, dir) > 0;
}

void ShadeSurface(RAB_LightSample lightSample, RAB_Surface surface, out float3 diffuse, out float3 specular) {
	diffuse = specular = 0;
	if (lightSample.SolidAnglePDF <= 0) return;
	const float3 L = normalize(lightSample.Position - surface.Position);
	if (dot(L, surface.GeometricNormal) <= 0) return;
	const float3 H = normalize(surface.ViewDirection + L);
	const float NoL = abs(dot(surface.Normal, L)), NoV = abs(dot(surface.Normal, surface.ViewDirection)), VoH = abs(dot(surface.ViewDirection, H)), NoH = abs(dot(surface.Normal, H));
	const float3 F = STL::BRDF::FresnelTerm(surface.Rf0, VoH), throughput = NoL * lightSample.Radiance / lightSample.SolidAnglePDF;
	diffuse = (1 - F) * surface.Albedo * STL::BRDF::DiffuseTerm(surface.Roughness, NoL, NoV, VoH) * throughput;
	specular = F * STL::BRDF::DistributionTerm(surface.Roughness, NoH) * STL::BRDF::GeometryTermMod(surface.Roughness, NoL, NoV, VoH, NoH) * throughput;
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample lightSample, RAB_Surface surface) {
	float3 diffuse, specular;
	ShadeSurface(lightSample, surface, diffuse, specular);
	return STL::Color::Luminance(diffuse + specular);
}

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo lightInfo, RAB_Surface surface, float2 uv) {
	TriangleLight triangleLight;
	triangleLight.Load(lightInfo);
	return triangleLight.CalculateSample(uv, surface.Position);
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample) {
	const float3 L = lightSample.Position - surface.Position;
	const float Llength = length(L);
	const RayDesc rayDesc = { surface.Position, 1e-3f, L / Llength, Llength - 1e-3f };
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
	TRACE_RAY(q, rayDesc, RAY_FLAG_NONE, ~0u);
	return q.CommittedStatus() == COMMITTED_NOTHING;
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, RAB_LightSample lightSample) { return RAB_GetConservativeVisibility(currentSurface, lightSample); }

float RAB_GetSurfaceBrdfPdf(RAB_Surface surface, float3 dir) {
	const float NoL = dot(surface.Normal, dir);
	if (NoL <= 0) return 0;
	const float
		NoV = abs(dot(surface.Normal, surface.ViewDirection)), NoH = abs(dot(surface.Normal, normalize(surface.ViewDirection + dir))),
		diffusePDF = STL::ImportanceSampling::Cosine::GetPDF(NoL),
		specularPDF = STL::ImportanceSampling::VNDF::GetPDF(STL::Geometry::RotateVector(STL::Geometry::GetBasis(surface.Normal), surface.ViewDirection), NoH, surface.Roughness);
	return lerp(specularPDF, diffusePDF, surface.DiffuseProbability);
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample lightSample, out float3 o_lightDir, out float o_lightDistance) {
	o_lightDir = lightSample.Position - surface.Position;
	o_lightDistance = length(o_lightDir);
	o_lightDir /= o_lightDistance;
}

bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax, out uint o_lightIndex, out float2 o_randXY) {
	const RayDesc rayDesc = { origin, tMin, direction, tMax };
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	TRACE_RAY(q, rayDesc, RAY_FLAG_NONE, ~0u);
	const bool hit = q.CommittedStatus() != COMMITTED_NOTHING;
	if (hit) {
		o_lightIndex = g_lightIndices[g_instanceData[q.CommittedInstanceIndex()].FirstGeometryIndex + q.CommittedGeometryIndex()];
		if (o_lightIndex != RTXDI_InvalidLightIndex) {
			o_lightIndex += q.CommittedPrimitiveIndex();
			o_randXY = Math::RandomFromBarycentrics(q.CommittedTriangleBarycentrics());
		}
	}
	else {
		o_lightIndex = RTXDI_InvalidLightIndex;
		o_randXY = 0;
	}
	return hit;
}

bool RAB_AreMaterialsSimilar(RAB_Surface a, RAB_Surface b) { return true; }
