module;

#include <DirectXMath.h>

export module Vertex;

using namespace DirectX;

export {
	struct VertexDesc { uint32_t Stride{}, NormalOffset = ~0u, TextureCoordinateOffset = ~0u, TangentOffset = ~0u; };

	struct VertexPositionNormalTextureTangent {
		XMFLOAT3 Position;
		uint32_t TextureCoordinate;
		XMFLOAT2 Normal, Tangent;
	};

	struct VertexPositionNormalTangentBones {
		XMFLOAT3 Position;
		float _;
		XMFLOAT2 Normal, Tangent;
		struct {
			uint32_t ID = ~0u;
			float Weight{};
		} Bones[4];
	};
}
