#pragma once

#include "Math.hlsli"

struct RAB_LightInfo { float3 Base, Edges[2], Radiance; };

inline RAB_LightInfo RAB_EmptyLightInfo() { return (RAB_LightInfo)0; }

struct RAB_LightSample {
	float3 Position, Normal, Radiance;
	float SolidAnglePDF;
};

inline RAB_LightSample RAB_EmptyLightSample() { return (RAB_LightSample)0; }
inline bool RAB_IsAnalyticLightSample(RAB_LightSample lightSample) { return false; }
inline float RAB_LightSampleSolidAnglePdf(RAB_LightSample lightSample) { return lightSample.SolidAnglePDF; }

struct TriangleLight {
	float3 Base, Edges[2], Normal, Radiance;
	float Area;

	void Load(RAB_LightInfo lightInfo) {
		Base = lightInfo.Base;
		Edges = lightInfo.Edges;
		Radiance = lightInfo.Radiance;
		const float3 normal = cross(lightInfo.Edges[0], lightInfo.Edges[1]);
		const float normalLength = length(normal);
		if (normalLength > 0) {
			Normal = normal / normalLength;
			Area = normalLength / 2;
		}
		else {
			Normal = 0;
			Area = 0;
		}
	}

	RAB_LightInfo Store() {
		RAB_LightInfo lightInfo;
		lightInfo.Base = Base;
		lightInfo.Edges = Edges;
		lightInfo.Radiance = Radiance;
		return lightInfo;
	}

	float CalculateSolidAnglePDF(float3 viewPosition, float3 lightSamplePosition, float3 lightSampleNormal) {
		const float3 L = lightSamplePosition - viewPosition;
		const float Llength = length(L);
		return Math::ToSolidAnglePDF(1 / Area, Llength, abs(dot(L / Llength, -lightSampleNormal)));
	}

	RAB_LightSample CalculateSample(float2 randomValue, float3 viewPosition) {
		RAB_LightSample lightSample;
		const float3 barycentrics = Math::SampleTriangle(randomValue);
		lightSample.Position = Base + Edges[0] * barycentrics.y + Edges[1] * barycentrics.z;
		lightSample.Normal = Normal;
		lightSample.Radiance = Radiance;
		lightSample.SolidAnglePDF = CalculateSolidAnglePDF(viewPosition, lightSample.Position, lightSample.Normal);
		return lightSample;
	}
};
