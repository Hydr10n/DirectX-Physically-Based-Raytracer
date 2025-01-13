module;

#include <filesystem>
#include <fstream>

#include <Windows.h>

#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"

#include "JsonHelpers.h"

export module MyScene;

export import Scene;

import ErrorHelpers;
import JsonConverters;
import ResourceHelpers;

using namespace DirectX;
using namespace ErrorHelpers;
using namespace nlohmann;
using namespace ResourceHelpers;
using namespace std;
using namespace std::filesystem;

using GamepadButtonState = GamePad::ButtonStateTracker::ButtonState;
using Key = Keyboard::Keys;

export {
	JSON_CONVERSION_FUNCTIONS(RenderObjectDesc, Name, Transform, IsVisible, Model, Animation);

	JSON_CONVERSION_FUNCTIONS(decltype(SceneDesc::Camera), Position, Rotation);
	JSON_CONVERSION_FUNCTIONS(decltype(SceneDesc::EnvironmentLightTexture), FilePath, Transform);
	JSON_CONVERSION_FUNCTIONS(SceneDesc, Camera, EnvironmentLightColor, EnvironmentLightTexture, Models, Animations, RenderObjects);

	struct MySceneDesc : SceneDesc {
		MySceneDesc(const path& filePath) {
			if (empty(filePath)) throw invalid_argument("Scene file path cannot be empty");

			const auto filePathString = filePath.string();

			ifstream file(filePath);
			ThrowIfFailed(static_cast<BOOL>(file.is_open()));

			try {
				ordered_json_f json;
				file >> json;
				reinterpret_cast<SceneDesc&>(*this) = json;

				for (const auto& renderObject : RenderObjects) {
					const auto CheckResources = [&](const char* name, const unordered_map<string, path>& resources, const string& URI) {
						if (!empty(URI) && !resources.contains(URI)) {
							auto renderObjectInfo = "RenderObject"s;
							if (empty(renderObject.Name)) renderObjectInfo = "Unnamed " + renderObjectInfo;
							else renderObjectInfo += " " + renderObject.Name;
							throw runtime_error(format("{}: {}: {} {} not found", filePathString, renderObjectInfo, name, URI));
						}
					};
					CheckResources("Models", Models, renderObject.Model);
					CheckResources("Animations", Animations, renderObject.Animation);
				}

				const auto ResolvePath = [&](path& path) {
					if (!empty(path) && !path.is_absolute()) path = filesystem::path(filePath).replace_filename(path);
				};
				const auto ResolvePaths = [&](unordered_map<string, path>& resources) {
					for (auto& Path : resources | views::values) ResolvePath(Path);
				};
				ResolvePath(EnvironmentLightTexture.FilePath);
				ResolvePaths(Models);
				ResolvePaths(Animations);
			}
			catch (const json::exception& e) { throw runtime_error(format("{}: {}", filePathString, e.what())); }
		}
	};

	struct MyScene : Scene {
		using Scene::Scene;

		bool IsStatic() const override { return !m_isAnimationPlaying || empty(AnimationCollections); }

		void Tick(double elapsedSeconds, const GamePad::ButtonStateTracker& gamepadStateTracker, const Keyboard::KeyboardStateTracker& keyboardStateTracker, const Mouse::ButtonStateTracker& mouseStateTracker) override {
			if (mouseStateTracker.GetLastState().positionMode == Mouse::MODE_RELATIVE) {
				if (gamepadStateTracker.a == GamepadButtonState::PRESSED) m_isAnimationPlaying = !m_isAnimationPlaying;
				if (keyboardStateTracker.IsKeyPressed(Key::Space)) m_isAnimationPlaying = !m_isAnimationPlaying;
			}

			if (IsStatic()) return;

			Tick(elapsedSeconds);

			Refresh();
		}

	protected:
		void Tick(double elapsedSeconds) override {
			for (auto& renderObject : RenderObjects) {
				if (!renderObject.IsVisible) continue;

				if (auto& animationCollection = renderObject.AnimationCollection; !empty(renderObject.Model.MeshNodes) && !empty(animationCollection)) {
					const auto selectedIndex = animationCollection.GetSelectedIndex();
					auto& animation = animationCollection[selectedIndex];
					const auto time = animation.GetTime();
					animation.Tick(elapsedSeconds);
					if (animation.GetTime() < time) animationCollection.SetSelectedIndex((selectedIndex + 1) % size(animationCollection));
				}
			}
		}

	private:
		bool m_isAnimationPlaying = true;
	};
}
