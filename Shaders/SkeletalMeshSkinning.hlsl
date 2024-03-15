#include "Vertex.hlsli"

#include "Math.hlsli"

cbuffer _ : register(b0) { uint g_vertexCount; }

StructuredBuffer<VertexPositionNormalTangentBones> g_skeletalVertices : register(t0);

struct Float3x4 { row_major float3x4 Matrix; };
StructuredBuffer<Float3x4> g_skeletalTransforms : register(t1);

RWStructuredBuffer<VertexPositionNormalTextureTangent> g_vertices : register(u0);
RWStructuredBuffer<float3> g_motionVectors : register(u1);

[RootSignature(
	"RootConstants(num32BitConstants=1, b0),"
	"SRV(t0),"
	"SRV(t1),"
	"UAV(u0),"
	"UAV(u1)"
)]
[numthreads(256, 1, 1)]
void main(uint vertexID : SV_DispatchThreadID) {
	if (vertexID >= g_vertexCount) return;

	float boneWeightSum = 0;
	float3x4 transform = 0;
	[unroll]
	for (uint i = 0; i < 4 && boneWeightSum < 1; i++) {
		const uint boneID = g_skeletalVertices[vertexID].Bones[i].ID;
		if (boneID == ~0u) break;
		const float boneWeight = g_skeletalVertices[vertexID].Bones[i].Weight;
		boneWeightSum += boneWeight;
		transform += boneWeight * g_skeletalTransforms[boneID].Matrix;
	}

	const float3 position = STL::Geometry::AffineTransform(transform, g_skeletalVertices[vertexID].Position);
	g_motionVectors[vertexID] = g_vertices[vertexID].Position - position;
	g_vertices[vertexID].Position = position;

	const float3x3 inverseTransposeTransform = Math::InverseTranspose((float3x3)transform);
	g_vertices[vertexID].Normal = normalize(STL::Geometry::RotateVector(inverseTransposeTransform, g_skeletalVertices[vertexID].Normal));
	g_vertices[vertexID].Tangent = normalize(STL::Geometry::RotateVector(inverseTransposeTransform, g_skeletalVertices[vertexID].Tangent));
}
