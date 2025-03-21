#include "RTXDIAppBridge.hlsli"
#include "Rtxdi/DI/Reservoir.hlsli"
#include "Rtxdi/ReGIR/ReGIRSampling.hlsli"

ROOT_SIGNATURE
[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
{
	const uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(globalIndex, g_graphicsSettings.RTXDI.Runtime.activeCheckerboardField);
	if (any(pixelPosition >= g_graphicsSettings.RenderSize))
	{
		return;
	}

	const RAB_Surface surface = RAB_GetGBufferSurface(pixelPosition, false);
	if (!RAB_IsSurfaceValid(surface))
	{
		return;
	}

	const ReSTIRDI_Parameters parameters = g_graphicsSettings.RTXDI.ReSTIRDI;

	RTXDI_DIReservoir reservoir = RTXDI_LoadDIReservoir(parameters.reservoirBufferParams, globalIndex, parameters.bufferIndices.shadingInputBufferIndex);
	if (!RTXDI_IsValidDIReservoir(reservoir))
	{
		return;
	}

	const RAB_LightInfo lightInfo = RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false);
	RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, surface, RTXDI_GetDIReservoirSampleUV(reservoir));

	if (parameters.shadingParams.enableFinalVisibility)
	{
		float3 visibility;
		bool visibilityReused = false;
		if (parameters.shadingParams.reuseFinalVisibility)
		{
			RTXDI_VisibilityReuseParameters visibilityReuseParameters;
			visibilityReuseParameters.maxAge = parameters.shadingParams.finalVisibilityMaxAge;
			visibilityReuseParameters.maxDistance = parameters.shadingParams.finalVisibilityMaxDistance;
			visibilityReused = RTXDI_GetDIReservoirVisibility(reservoir, visibilityReuseParameters, visibility);
		}
		if (!visibilityReused)
		{
			visibility = GetFinalVisibility(surface, lightSample.Position);
			RTXDI_StoreVisibilityInDIReservoir(reservoir, visibility, parameters.temporalResamplingParams.discardInvisibleSamples);
			RTXDI_StoreDIReservoir(reservoir, parameters.reservoirBufferParams, globalIndex, parameters.bufferIndices.shadingInputBufferIndex);
		}

		if (all(visibility == 0))
		{
			return;
		}

		lightSample.Radiance *= visibility;
	}

	lightSample.Radiance *= RTXDI_GetDIReservoirInvPdf(reservoir);

	float3 diffuse, specular;
	surface.Shade(lightSample, diffuse, specular);

	const float3 radiance = diffuse + specular;
	if (all(radiance == 0) || any(!isfinite(radiance)))
	{
		return;
	}

	if (g_graphicsSettings.IsReGIRCellVisualizationEnabled
		&& parameters.initialSamplingParams.localLightSamplingMode == ReSTIRDI_LocalLightSamplingMode_REGIR_RIS)
	{
		g_radiance[pixelPosition] += RTXDI_VisualizeReGIRCells(g_graphicsSettings.RTXDI.ReGIR, surface.Position);

		return;
	}

	const float lightDistance = length(lightSample.Position - surface.Position);
	if (g_graphicsSettings.IsLastRenderPass)
	{
		if (g_graphicsSettings.Denoiser == Denoiser::None)
		{
			g_radiance[pixelPosition] += radiance;
		}
		else if (g_graphicsSettings.Denoiser == Denoiser::DLSSRayReconstruction)
		{
			g_radiance[pixelPosition] += radiance;

			if (any(specular > 0))
			{
				g_specularHitDistance[pixelPosition] = lightDistance;
			}
		}
		else
		{
			g_diffuse[pixelPosition] = float4(diffuse, lightDistance);
			g_specular[pixelPosition] = float4(specular, lightDistance);
		}
	}
	else
	{
		g_diffuse[pixelPosition] = float4(diffuse, lightDistance);
		g_specular[pixelPosition] = float4(specular, lightDistance);
	}
}
