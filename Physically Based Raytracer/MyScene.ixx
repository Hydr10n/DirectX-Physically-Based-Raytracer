module;

#include <DirectXMath.h>

#include <filesystem>

export module MyScene;

import Scene;

using namespace DirectX;
using namespace std;
using namespace std::filesystem;

export struct MyScene : Scene {
	MyScene() {
		Camera = {
			.Position{ -5.948547f, 5.7683268f, -27.316963f },
			.Directions{
				.Up{ 0.11510569f, 0.95278513f, 0.28098264f },
				.Forward{ 0.36118266f, -0.30364546f, 0.88167256f }
			}
		};

		BuildModels();

		BuildRenderItems();

		EnvironmentLightColor = {};
	}

private:
	void BuildModels() {
		decltype (Models) models;

		models.DirectoryPath = path(*__wargv).replace_filename(LR"(Assets\Models)");

		const auto BuildModel = [&](const string& name, const path& filePath) {
			auto& model = models[name];
			model = make_shared<Model>();
			model->BoundingBox.Extents = { 5, 5, 5 };
			model->FilePath = filePath;
			model->LoaderFlags = Model::LoaderFlags::AdjustCenter;
		};

		BuildModel("1", LR"(Diorama of Cyberpunk City\scene.gltf)");
		BuildModel("2", LR"(Cyberpunk City\scene.gltf)");
		BuildModel("3", LR"(Cyberpunk Car\scene.gltf)");

		Models = move(models);
	}

	void BuildRenderItems() {
		decltype(RenderItems) renderItems;

		renderItems.emplace_back(RenderItem{ .pModel = &Models.at("1"), .Transform = XMMatrixRotationY(-XM_PI) * XMMatrixTranslation(4.5f, 0, 0) });
		renderItems.emplace_back(RenderItem{ .pModel = &Models.at("2"), .Transform = XMMatrixRotationY(-XM_PIDIV2) * XMMatrixTranslation(-4.5f, -0.7f, -0.6f) });
		renderItems.emplace_back(RenderItem{ .pModel = &Models.at("3"), .Transform = XMMatrixRotationY(-XM_PI) * XMMatrixTranslation(4.3f, 0, -10) });

		RenderItems = move(renderItems);
	}
};
