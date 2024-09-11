module;

#include <DirectXMath.h>

#include "ml.h"
#include "ml.hlsli"

export module Vertex;

using namespace DirectX;
using namespace Packing;

namespace {
	auto EncodeUnitVector(const XMFLOAT3& value) {
		return reinterpret_cast<const XMFLOAT2&>(::EncodeUnitVector(reinterpret_cast<const float3&>(value), true));
	}
}

export {
	struct VertexDesc { uint32_t Stride{}, NormalOffset = ~0u, TextureCoordinateOffset = ~0u, TangentOffset = ~0u; };

	struct VertexPositionNormalTextureTangent {
		XMFLOAT3 Position;
		uint32_t TextureCoordinate;
		XMFLOAT2 Normal, Tangent;

		void StoreTextureCoordinate(XMFLOAT2 value) {
			TextureCoordinate = reinterpret_cast<const uint32_t&>(float2_to_float16_t2(reinterpret_cast<const float2&> (value)));
		}

		void StoreNormal(const XMFLOAT3& value) { Normal = EncodeUnitVector(value); }

		void StoreTangent(const XMFLOAT3& value) { Tangent = EncodeUnitVector(value); }
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
