module;

#include <DirectXMath.h>

#include <filesystem>

export module Scene;

import Model;

using namespace DirectX;
using namespace std;
using namespace std::filesystem;

export {
	struct RenderItem {
		string Name;

		bool IsVisible = true;

		shared_ptr<Model>* pModel{};

		XMMATRIX Transform = XMMatrixIdentity();
	};

	struct Scene {
		virtual ~Scene() = default;

		struct {
			XMFLOAT3 Position;
			struct { XMFLOAT3 Up{ 0, 1, 0 }, Forward{ 0, 0, 1 }; } Directions;
		} Camera;

		ModelDictionary Models;

		vector<RenderItem> RenderItems;

		XMFLOAT4 EnvironmentLightColor{ 0, 0, 0, -1 }, EnvironmentColor{ 0, 0, 0, -1 };

		path EnvironmentCubeMapDirectoryPath;

		struct {
			path FilePath;
			XMMATRIX Transform = XMMatrixIdentity();
		} EnvironmentLightCubeMap, EnvironmentCubeMap;
	};
}
