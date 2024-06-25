module;

#include <DirectXMath.h>

export module Material;

using namespace DirectX;

export {
	enum class AlphaMode { Opaque, Blend, Mask };

	struct Material {
		XMFLOAT4 BaseColor{};
		XMFLOAT3 EmissiveColor{};
		float Metallic{}, Roughness = 0.5f, Transmission{}, IOR = 1;
		AlphaMode AlphaMode = AlphaMode::Opaque;
		float AlphaThreshold = 0.5f;
		XMFLOAT3 _;
	};
}
