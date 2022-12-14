module;

#include <Windows.h>

#include <DirectXMath.h>

export module ShaderCommonData;

import Material;

using namespace DirectX;

export {
	struct GlobalResourceDescriptorHeapIndices {
		UINT
			LocalResourceDescriptorHeapIndices = ~0u,
			Camera = ~0u,
			GlobalData = ~0u, LocalData = ~0u,
			Output = ~0u,
			EnvironmentLightCubeMap = ~0u, EnvironmentCubeMap = ~0u;
		UINT _;
	};

	struct LocalResourceDescriptorHeapIndices {
		struct {
			UINT Vertices = ~0u, Indices = ~0u;
			XMUINT2 _;
		} TriangleMesh;
		struct { UINT BaseColorMap = ~0u, EmissiveMap = ~0u, SpecularMap = ~0u, MetallicMap = ~0u, RoughnessMap = ~0u, AmbientOcclusionMap = ~0u, OpacityMap = ~0u, NormalMap = ~0u; } Textures;
	};

	struct GlobalData {
		UINT RaytracingMaxTraceRecursionDepth, RaytracingSamplesPerPixel, FrameCount, AccumulatedFrameIndex;
		XMFLOAT4 EnvironmentLightColor;
		XMMATRIX EnvironmentLightCubeMapTransform;
		XMFLOAT4 EnvironmentColor;
		XMMATRIX EnvironmentCubeMapTransform;
	};

	struct LocalData {
		BOOL HasPerVertexTangents;
		XMUINT3 _;
		Material Material;
		XMMATRIX TextureTransform;
	};
}
