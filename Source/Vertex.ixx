module;

#include "VertexTypes.h"

export module Vertex;

using namespace DirectX;

export {
	struct VertexDesc { UINT Stride{}, NormalOffset = ~0u, TextureCoordinateOffset = ~0u, TangentOffset = ~0u; };

	struct VertexPositionNormalTextureTangent : VertexPositionNormalTexture { XMFLOAT3 tangent; };

	struct VertexPositionNormalTangentBones : VertexPositionNormal {
		XMFLOAT3 tangent;

		struct {
			UINT ID = ~0u;
			float Weight{};
		} bones[4];
	};
}
