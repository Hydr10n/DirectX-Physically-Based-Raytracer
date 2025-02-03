#include "Vertex.hlsli"

#include "Math.hlsli"

cbuffer _ : register(b0)
{
	uint g_vertexCount;
}

StructuredBuffer<VertexPositionNormalTangentSkin> g_skeletalVertices : register(t0);

struct Float3x4
{
	row_major float3x4 Matrix;
};
StructuredBuffer<Float3x4> g_skeletalTransforms : register(t1);

RWStructuredBuffer<VertexPositionNormalTextureTangent> g_vertices : register(u0);
RWStructuredBuffer<uint2> g_motionVectors : register(u1);

[RootSignature(
	"RootConstants(num32BitConstants=1, b0),"
	"SRV(t0),"
	"SRV(t1),"
	"UAV(u0),"
	"UAV(u1)"
)]
[numthreads(256, 1, 1)]
void main(uint vertexIndex : SV_DispatchThreadID)
{
	if (vertexIndex >= g_vertexCount)
	{
		return;
	}

	float3x4 transform = 0;
	const VertexPositionNormalTangentSkin skeletalVertex = g_skeletalVertices[vertexIndex];
	const float weights[] =
	{
		skeletalVertex.Weights[0],
		skeletalVertex.Weights[1],
		skeletalVertex.Weights[2],
		1 - skeletalVertex.Weights[0] - skeletalVertex.Weights[1] - skeletalVertex.Weights[2]
	};
	[unroll]
	for (uint i = 0; i < 4; i++)
	{
		const uint joint = skeletalVertex.Joints[i];
		transform += weights[i] * g_skeletalTransforms[joint].Matrix;
	}

	const float3
		position = Geometry::AffineTransform(transform, skeletalVertex.Position),
		motionVector = g_vertices[vertexIndex].Position - position,
		normal = Packing::DecodeUnitVector(skeletalVertex.Normal, true, false),
		tangent = Packing::DecodeUnitVector(skeletalVertex.Tangent, true, false);
	const float3x3 rotation = (float3x3)transform;
	g_vertices[vertexIndex].Position = position;
	g_vertices[vertexIndex].Normal = Packing::EncodeUnitVector(normalize(Geometry::RotateVector(Math::InverseTranspose(rotation), normal)), true);
	g_vertices[vertexIndex].Tangent = Packing::EncodeUnitVector(normalize(Geometry::RotateVector(rotation, tangent)), true);
	g_motionVectors[vertexIndex] = uint2(Packing::Rg16fToUint(motionVector.xy), Packing::Rg16fToUint(float2(motionVector.z, 0)));
}
