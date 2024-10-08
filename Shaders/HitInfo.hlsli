#pragma once

#include "Vertex.hlsli"

#include "SelfIntersectionAvoidance.hlsli"

struct HitInfo
{
	float3 Position, Normal;
	float2 TextureCoordinate;

	float3 ObjectPosition, GeometricNormal;

	float3 SafeNormal;
	float SafeOffset;

	float2 Barycentrics;

	bool IsFrontFace;

	float Distance;

	uint InstanceIndex, ObjectIndex, PrimitiveIndex;

	void Initialize(
		float3 positions[3], float3 normals[3], float2 textureCoordinates[3],
		float2 barycentrics,
		float3x4 objectToWorld, float3x4 worldToObject,
		float3 worldRayDirection, float distance
	)
	{
		float3 objectNormal;
		SelfIntersectionAvoidance::GetSafeTriangleSpawnPoint(
			ObjectPosition, Position, objectNormal, SafeNormal, SafeOffset,
			positions, barycentrics, objectToWorld, worldToObject
		);
		Normal = normalize(Geometry::RotateVector(transpose((float3x3)worldToObject), Vertex::Interpolate(normals, barycentrics)));
		if (!(IsFrontFace = dot(Normal, worldRayDirection) < 0))
		{
			Normal = -Normal;
		}
		GeometricNormal = Normal;
		TextureCoordinate = Vertex::Interpolate(textureCoordinates, barycentrics);
		Barycentrics = barycentrics;
		Distance = distance;
	}

	float3 GetSafeWorldRayOrigin(float3 worldRayDirection)
	{
		return SelfIntersectionAvoidance::OffsetSpawnPoint(Position, SafeNormal * Math::Sign(dot(worldRayDirection, SafeNormal)), SafeOffset);
	}
};
