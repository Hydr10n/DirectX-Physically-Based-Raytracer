module;

#include <filesystem>

#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"
#include "directxtk12/SimpleMath.h"

#include "rtxmu/D3D12AccelStructManager.h"

export module Scene;

import CommandList;
import DeviceContext;
import GLTFHelpers;
import Math;
import RaytracingHelpers;
import ResourceHelpers;
import SkeletalMeshSkinning;
import TextureHelpers;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace DirectX::SimpleMath;
using namespace Math;
using namespace ResourceHelpers;
using namespace std;
using namespace std::filesystem;
using namespace TextureHelpers;

export {
	struct RenderObjectBase {
		virtual ~RenderObjectBase() = default;

		string Name;

		AffineTransform Transform;

		bool IsVisible = true;
	};

	struct RenderObjectDesc : RenderObjectBase { string Model, Animation; };

	struct RenderObject : RenderObjectBase {
		Model Model;

		AnimationCollection AnimationCollection;
	};

	struct SceneBase {
		struct {
			Vector3 Position;
			Quaternion Rotation;
		} Camera;

		struct EnvironmentLightBase {
			Color Color{ 0, 0, 0, -1 };
			Quaternion Rotation;
		};

		virtual ~SceneBase() = default;
	};

	struct SceneDesc : SceneBase {
		struct : EnvironmentLightBase {
			path Texture;
		} EnvironmentLight;

		unordered_map<string, path> Models, Animations;

		vector<RenderObjectDesc> RenderObjects;
	};

	struct Scene : SceneBase {
		struct InstanceData {
			uint32_t FirstGeometryIndex;
			XMFLOAT3X4 PreviousObjectToWorld, ObjectToWorld;
		};

		struct : EnvironmentLightBase {
			shared_ptr<Texture> Texture;
		} EnvironmentLight;

		struct ModelDictionaryLoader {
			void operator()(Model& resource, const path& filePath, const DeviceContext& deviceContext) const {
				CommandList commandList(deviceContext);
				commandList.Begin();

				GLTFHelpers::LoadModel(resource, filePath, commandList, true);

				commandList.End();
			}
		};
		ResourceDictionary<string, Model, ModelDictionaryLoader> Models;

		struct AnimationCollectionDictionaryLoader {
			void operator()(AnimationCollection& resource, const path& filePath) const {
				GLTFHelpers::LoadAnimation(resource, filePath);
			}
		};
		ResourceDictionary<string, AnimationCollection, AnimationCollectionDictionaryLoader> AnimationCollections;

		vector<RenderObject> RenderObjects;

		explicit Scene(const DeviceContext& deviceContext) : m_deviceContext(deviceContext), m_skeletalMeshSkinning(deviceContext) {}

		~Scene() override {
			vector<uint64_t> IDs;
			IDs.reserve(size(m_bottomLevelAccelerationStructureIDs) + 1);
			for (const auto& [MeshNode, ID] : m_bottomLevelAccelerationStructureIDs) {
				IDs.emplace_back(ID.first);
				MeshNode->OnDestroyed.remove(ID.second);
			}
			if (m_topLevelAccelerationStructure.ID != ~0ull) {
				IDs.emplace_back(m_topLevelAccelerationStructure.ID);
			}
			m_deviceContext.AccelerationStructureManager->RemoveAccelerationStructures(IDs);
			m_bottomLevelAccelerationStructureIDs = {};
			m_topLevelAccelerationStructure = {};

			CollectGarbage();
		}

		virtual bool IsStatic() const { return false; }

		virtual void Tick(double elapsedSeconds, const GamePad::ButtonStateTracker& gamepadStateTracker, const Keyboard::KeyboardStateTracker& keyboardStateTracker, const Mouse::ButtonStateTracker& mouseStateTracker) = 0;

		void Load(const SceneDesc& sceneDesc) {
			reinterpret_cast<SceneBase&>(*this) = sceneDesc;

			CommandList commandList(m_deviceContext);
			commandList.Begin();

			reinterpret_cast<EnvironmentLightBase&>(EnvironmentLight) = sceneDesc.EnvironmentLight;
			if (!empty(sceneDesc.EnvironmentLight.Texture)) {
				EnvironmentLight.Texture = LoadTexture(commandList, ResolveResourcePath(sceneDesc.EnvironmentLight.Texture), true);
				EnvironmentLight.Texture->CreateSRV();
			}

			{
				unordered_map<string, path> modelDescs, animationDescs;
				for (const auto renderObject : sceneDesc.RenderObjects) {
					if (!empty(renderObject.Model)) {
						modelDescs.try_emplace(renderObject.Model, sceneDesc.Models.at(renderObject.Model));
					}

					if (!empty(renderObject.Animation)) {
						animationDescs.try_emplace(renderObject.Animation, sceneDesc.Animations.at(renderObject.Animation));
					}
				}

				Models.Load(modelDescs, true, 8, m_deviceContext);

				AnimationCollections.Load(animationDescs, true, 8);

				for (const auto& renderObjectDesc : sceneDesc.RenderObjects) {
					RenderObject renderObject;
					reinterpret_cast<RenderObjectBase&>(renderObject) = renderObjectDesc;

					if (!empty(renderObjectDesc.Model)) {
						renderObject.Model = Model(*Models.at(renderObjectDesc.Model), commandList);
					}

					if (!empty(renderObjectDesc.Animation)) {
						renderObject.AnimationCollection = *AnimationCollections.at(renderObjectDesc.Animation);
						renderObject.AnimationCollection.Bind(renderObject.Model.SkinJoints);
					}

					RenderObjects.emplace_back(renderObject);
				}
			}

			Tick(0);

			Refresh();

			SkinSkeletalMeshes(commandList);

			CreateAccelerationStructures(commandList);

			commandList.End();

			commandList.Begin();

			commandList.CompactAccelerationStructures();

			commandList.End();
		}

		const auto& GetInstanceData() const noexcept { return m_instanceData; }

		auto GetObjectCount() const noexcept { return m_objectCount; }

		void Refresh() {
			uint32_t instanceIndex = 0, objectIndex = 0;
			for (const auto& renderObject : RenderObjects) {
				for (const auto& meshNode : renderObject.Model.MeshNodes) {
					const auto Transform = [&] {
						const auto ZFlip = Matrix::CreateScale(1, 1, -1);
						const auto To3x4 = [](const Matrix& matrix) {
							XMFLOAT3X4 ret;
							XMStoreFloat3x4(&ret, matrix);
							return ret;
						};
						if (const auto& animationCollection = renderObject.AnimationCollection; !empty(animationCollection)) {
							const auto& globalTransforms = animationCollection[animationCollection.GetSelectedIndex()].GetGlobalTransforms();
							if (const auto pGlobalTransform = globalTransforms.find(meshNode->NodeName);
								pGlobalTransform != cend(globalTransforms)) {
								return To3x4(pGlobalTransform->second * ZFlip * renderObject.Transform());
							}
						}
						return To3x4(meshNode->GlobalTransform * ZFlip * renderObject.Transform());
					};
					InstanceData instanceData;
					instanceData.FirstGeometryIndex = objectIndex;
					if (instanceIndex < size(m_instanceData)) {
						instanceData.PreviousObjectToWorld = m_instanceData[instanceIndex].ObjectToWorld;
						instanceData.ObjectToWorld = Transform();
						m_instanceData[instanceIndex] = instanceData;
					}
					else {
						instanceData.PreviousObjectToWorld = instanceData.ObjectToWorld = Transform();
						m_instanceData.emplace_back(instanceData);
					}
					instanceIndex++;
					objectIndex += static_cast<uint32_t>(size(meshNode->Meshes));
				}
			}
			m_objectCount = objectIndex;
		}

		void SkinSkeletalMeshes(CommandList& commandList) {
			auto prepared = false;
			for (const auto& renderObject : RenderObjects) {
				if (!renderObject.IsVisible) {
					continue;
				}

				const auto& animationCollection = renderObject.AnimationCollection;
				if (empty(animationCollection) || !animationCollection.IsSkinned()) {
					continue;
				}

				const auto& skeletalTransforms = animationCollection[animationCollection.GetSelectedIndex()].GetSkeletalTransforms();
				if (empty(skeletalTransforms)) {
					continue;
				}

				for (const auto& meshNode : renderObject.Model.MeshNodes) {
					const auto pSkeletalTransforms = skeletalTransforms.find(meshNode->NodeName);
					if (pSkeletalTransforms == cend(skeletalTransforms)) {
						continue;
					}

					commandList.Copy(*meshNode->SkeletalTransforms, pSkeletalTransforms->second);

					for (const auto& mesh : meshNode->Meshes) {
						if (!mesh->SkeletalVertices) {
							continue;
						}

						if (!prepared) {
							m_skeletalMeshSkinning.Prepare(commandList);

							prepared = true;
						}

						m_skeletalMeshSkinning.GPUBuffers = {
							.SkeletalVertices = mesh->SkeletalVertices.get(),
							.SkeletalTransforms = meshNode->SkeletalTransforms.get(),
							.Vertices = mesh->Vertices.get(),
							.MotionVectors = mesh->MotionVectors.get()
						};

						m_skeletalMeshSkinning.Process(commandList);
					}
				}
			}
		}

		auto GetTopLevelAccelerationStructure() const {
			return m_deviceContext.AccelerationStructureManager->GetAccelStructGPUVA(m_topLevelAccelerationStructure.ID);
		}

		void CreateAccelerationStructures(CommandList& commandList) {
			auto& accelerationStructureManager = *m_deviceContext.AccelerationStructureManager;

			{
				vector<vector<D3D12_RAYTRACING_GEOMETRY_DESC>> geometryDescs;
				vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> newInputs, updatedInputs;
				vector<MeshNode*> newMeshNodes;
				vector<uint64_t> updatedIDs;

				for (const auto& renderObject : RenderObjects) {
					const auto& model = renderObject.Model;

					const auto& animationCollection = renderObject.AnimationCollection;
					const auto isAnimated = !empty(animationCollection)
						&& animationCollection.IsSkinned()
						&& !empty(animationCollection[animationCollection.GetSelectedIndex()].GetSkeletalTransforms());

					for (const auto& meshNode : model.MeshNodes) {
						auto isSkeletal = false;
						for (const auto& mesh : meshNode->Meshes) {
							if (mesh->SkeletalVertices) {
								isSkeletal = true;
								break;
							}
						}

						const auto [first, second] = m_bottomLevelAccelerationStructureIDs.try_emplace(meshNode.get());
						if (!second && (!renderObject.IsVisible || !isAnimated || !isSkeletal)) {
							continue;
						}

						auto& _geometryDescs = geometryDescs.emplace_back();
						_geometryDescs.reserve(size(meshNode->Meshes));
						for (const auto& mesh : meshNode->Meshes) {
							_geometryDescs.emplace_back(CreateGeometryDesc(
								*mesh->Vertices, *mesh->Indices,
								mesh->MaterialIndex == ~0u || model.Materials[mesh->MaterialIndex].AlphaMode == AlphaMode::Opaque ?
								D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
							));
						}

						if (const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{
							.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
							.Flags = isSkeletal ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | (!second && isAnimated ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE) : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION,
							.NumDescs = static_cast<UINT>(size(_geometryDescs)),
							.pGeometryDescs = data(_geometryDescs)
							};
							second) {
							newInputs.emplace_back(inputs);
							newMeshNodes.emplace_back(meshNode.get());
						}
						else {
							updatedInputs.emplace_back(inputs);
							updatedIDs.emplace_back(first->second.first);
						}
					}
				}

				if (!empty(newInputs)) {
					const auto IDs = commandList.BuildAccelerationStructures(newInputs);

					for (size_t i = 0; const auto & meshNode : newMeshNodes) {
						auto& ID = m_bottomLevelAccelerationStructureIDs.at(meshNode);
						ID.first = IDs[i++];
						ID.second = meshNode->OnDestroyed.append([&](MeshNode* pMeshNode) {
							if (const auto pID = m_bottomLevelAccelerationStructureIDs.find(pMeshNode);
							pID != cend(m_bottomLevelAccelerationStructureIDs)) {
							m_unreferencedBottomLevelAccelerationStructureIDs.emplace_back(pID->second.first);
							m_bottomLevelAccelerationStructureIDs.erase(pID);
						}
							});
					}
				}

				if (!empty(updatedInputs)) {
					commandList.UpdateAccelerationStructures(updatedInputs, updatedIDs);
				}
			}

			vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
			instanceDescs.reserve(size(m_instanceData));
			for (uint32_t instanceIndex = 0; const auto & renderObject : RenderObjects) {
				for (const auto& meshNode : renderObject.Model.MeshNodes) {
					const auto& instanceData = m_instanceData[instanceIndex++];
					auto& instanceDesc = instanceDescs.emplace_back(D3D12_RAYTRACING_INSTANCE_DESC{
						.InstanceID = instanceData.FirstGeometryIndex,
						.InstanceMask = renderObject.IsVisible ? ~0u : 0,
						.InstanceContributionToHitGroupIndex = instanceData.FirstGeometryIndex,
						.AccelerationStructure = accelerationStructureManager.GetAccelStructGPUVA(m_bottomLevelAccelerationStructureIDs.at(meshNode.get()).first)
						});
					reinterpret_cast<XMFLOAT3X4&>(instanceDesc.Transform) = instanceData.ObjectToWorld;
				}
			}
			BuildTopLevelAccelerationStructure(commandList, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, instanceDescs, false, m_topLevelAccelerationStructure);
		}

		void CollectGarbage() {
			if (!empty(m_unreferencedBottomLevelAccelerationStructureIDs)) {
				m_deviceContext.AccelerationStructureManager->RemoveAccelerationStructures(m_unreferencedBottomLevelAccelerationStructureIDs);
				m_unreferencedBottomLevelAccelerationStructureIDs.clear();
			}
		}

	protected:
		virtual void Tick(double elapsedSeconds) = 0;

	private:
		const DeviceContext& m_deviceContext;

		SkeletalMeshSkinning m_skeletalMeshSkinning;

		vector<InstanceData> m_instanceData;
		uint32_t m_objectCount{};

		vector<uint64_t> m_unreferencedBottomLevelAccelerationStructureIDs;
		unordered_map<MeshNode*, pair<uint64_t, MeshNode::DestroyEvent::Handle>> m_bottomLevelAccelerationStructureIDs;
		TopLevelAccelerationStructure m_topLevelAccelerationStructure;
	};
}
