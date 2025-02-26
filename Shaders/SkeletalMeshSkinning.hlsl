#include "Vertex.hlsli"

#include "Math.hlsli"

cbuffer _ : register(b0)
{
	uint g_vertexCount;
}

StructuredBuffer<VertexPositionNormalTangentSkin> g_skeletalVertices : register(t0);

struct row_major_float3x4
{
	row_major float3x4 Value;
};
StructuredBuffer<row_major_float3x4> g_skeletalTransforms : register(t1);

RWStructuredBuffer<VertexPositionNormalTangentTexture> g_vertices : register(u0);
RWStructuredBuffer<float16_t4> g_motionVectors : register(u1);

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
		transform += weights[i] * g_skeletalTransforms[joint].Value;
	}

	const float3
		position = Geometry::AffineTransform(transform, skeletalVertex.Position),
		motionVector = g_vertices[vertexIndex].Position - position,
		normal = Unpack_R16G16B16_SNORM(skeletalVertex.Normal),
		tangent = Unpack_R16G16B16_SNORM(skeletalVertex.Tangent);
	const float3x3 rotation = (float3x3)transform;
	g_vertices[vertexIndex].Position = position;
	g_vertices[vertexIndex].Normal = Pack_R16G16B16_SNORM(normalize(Geometry::RotateVector(Math::InverseTranspose(rotation), normal)));
	g_vertices[vertexIndex].Tangent = Pack_R16G16B16_SNORM(normalize(Geometry::RotateVector(rotation, tangent)));
	g_motionVectors[vertexIndex].xyz = (float16_t3)motionVector;
}
