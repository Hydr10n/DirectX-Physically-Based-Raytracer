module;

#include <filesystem>

#include "directxtk12/GamePad.h"
#include "directxtk12/Keyboard.h"
#include "directxtk12/Mouse.h"
#include "directxtk12/ResourceUploadBatch.h"
#include "directxtk12/SimpleMath.h"

#include "rtxmu/D3D12AccelStructManager.h"

export module Scene;

import Animation;
import CommandList;
import DescriptorHeap;
import Event;
import Math;
import Model;
import RaytracingHelpers;
import ResourceHelpers;
import SkeletalMeshSkinning;
import Texture;

using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace DirectX::SimpleMath;
using namespace Math;
using namespace ResourceHelpers;
using namespace rtxmu;
using namespace std;
using namespace std::filesystem;

export {
	struct RenderObjectBase {
		virtual ~RenderObjectBase() = default;

		string Name;

		Transform Transform;

		bool IsVisible = true;
	};

	struct RenderObjectDesc : RenderObjectBase { string ModelURI, AnimationURI; };

	struct RenderObject : RenderObjectBase {
		Model Model;

		AnimationCollection AnimationCollection;
	};

	struct SceneBase {
		SceneBase() = default;

		virtual ~SceneBase() = default;

		struct {
			Vector3 Position;
			Quaternion Rotation;
		} Camera;

		Color EnvironmentLightColor{ 0, 0, 0, -1 }, EnvironmentColor{ 0, 0, 0, -1 };
	};

	struct SceneDesc : SceneBase {
		struct {
			path FilePath;
			Transform Transform;
		} EnvironmentLightTexture, EnvironmentTexture;

		unordered_map<string, path> Models, Animations;

		vector<RenderObjectDesc> RenderObjects;
	};

	struct Scene : SceneBase {
		struct InstanceData {
			UINT FirstGeometryIndex;
			XMFLOAT3X4 PreviousObjectToWorld, ObjectToWorld;
		};

		struct : Texture { Transform Transform; } EnvironmentLightTexture, EnvironmentTexture;

		ModelDictionary Models;

		AnimationCollectionDictionary AnimationCollections;

		vector<RenderObject> RenderObjects;

		Scene(ID3D12Device5* pDevice, ID3D12CommandQueue* pCommandQueue) noexcept(false) : m_device(pDevice), m_commandQueue(pCommandQueue), m_commandList(pDevice), m_skeletalMeshSkinning(pDevice), m_observer(*this) {}

		virtual bool IsStatic() const { return false; }

		virtual void Tick(double elapsedSeconds, const GamePad::ButtonStateTracker& gamepadStateTracker, const Keyboard::KeyboardStateTracker& keyboardStateTracker, const Mouse::ButtonStateTracker& mouseStateTracker) {}

		void Load(const SceneDesc& sceneDesc, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) {
			reinterpret_cast<SceneBase&>(*this) = sceneDesc;

			{
				ResourceUploadBatch resourceUploadBatch(m_device);
				resourceUploadBatch.Begin();

				if (!empty(sceneDesc.EnvironmentLightTexture.FilePath)) {
					EnvironmentLightTexture.Load(ResolveResourcePath(sceneDesc.EnvironmentLightTexture.FilePath), m_device, resourceUploadBatch, descriptorHeap, descriptorHeapIndex);
					EnvironmentLightTexture.Transform = sceneDesc.EnvironmentLightTexture.Transform;
				}

				if (!empty(sceneDesc.EnvironmentTexture.FilePath)) {
					EnvironmentTexture.Load(ResolveResourcePath(sceneDesc.EnvironmentTexture.FilePath), m_device, resourceUploadBatch, descriptorHeap, descriptorHeapIndex);
					EnvironmentTexture.Transform = sceneDesc.EnvironmentTexture.Transform;
				}

				resourceUploadBatch.End(m_commandQueue).get();
			}

			{
				unordered_map<string, path> modelDescs, animationDescs;
				for (const auto renderObject : sceneDesc.RenderObjects) {
					if (!empty(renderObject.ModelURI)) modelDescs.try_emplace(renderObject.ModelURI, sceneDesc.Models.at(renderObject.ModelURI));

					if (!empty(renderObject.AnimationURI)) animationDescs.try_emplace(renderObject.AnimationURI, sceneDesc.Animations.at(renderObject.AnimationURI));
				}

				Models.Load(modelDescs, true, 8, m_device, m_commandQueue, descriptorHeap, descriptorHeapIndex);

				AnimationCollections.Load(animationDescs, true, 8);

				for (const auto& renderObjectDesc : sceneDesc.RenderObjects) {
					RenderObject renderObject;
					reinterpret_cast<RenderObjectBase&>(renderObject) = renderObjectDesc;

					if (!empty(renderObjectDesc.ModelURI)) renderObject.Model = Model(*Models.at(renderObjectDesc.ModelURI), m_device, m_commandQueue, descriptorHeap, descriptorHeapIndex);

					if (!empty(renderObjectDesc.AnimationURI)) {
						renderObject.AnimationCollection = *AnimationCollections.at(renderObjectDesc.AnimationURI);
						renderObject.AnimationCollection.SetBoneInfo(renderObject.Model.BoneInfo);
					}

					RenderObjects.emplace_back(renderObject);
				}
			}

			Refresh();

			CreateAccelerationStructures(false);
		}

		const auto& GetInstanceData() const noexcept { return m_instanceData; }

		auto GetObjectCount() const noexcept { return m_objectCount; }

		void Refresh() {
			UINT instanceIndex = 0, objectIndex = 0;
			for (const auto& renderObject : RenderObjects) {
				const auto& meshNodes = renderObject.Model.MeshNodes;
				for (const auto& meshNode : meshNodes) {
					const auto Transform = [&] {
						const auto To3x4 = [](const Matrix& matrix) {
							XMFLOAT3X4 ret;
							XMStoreFloat3x4(&ret, matrix);
							return ret;
						};
						if (const auto& animationCollection = renderObject.AnimationCollection; !empty(animationCollection)) {
							const auto& globalTransforms = animationCollection[animationCollection.GetSelectedIndex()].GetGlobalTransforms();
							if (const auto pGlobalTransform = globalTransforms.find(meshNode->Name); pGlobalTransform != cend(globalTransforms)) {
								return To3x4(pGlobalTransform->second * renderObject.Transform());
							}
						}
						return To3x4(meshNode->GlobalTransform * renderObject.Transform());
					};
					InstanceData instanceData;
					instanceData.FirstGeometryIndex = objectIndex;
					if (instanceIndex >= size(m_instanceData)) {
						instanceData.PreviousObjectToWorld = instanceData.ObjectToWorld = Transform();
						m_instanceData.emplace_back(instanceData);
					}
					else {
						instanceData.PreviousObjectToWorld = m_instanceData[instanceIndex].ObjectToWorld;
						instanceData.ObjectToWorld = Transform();
						m_instanceData[instanceIndex] = instanceData;
					}
					instanceIndex++;
					objectIndex += static_cast<UINT>(size(meshNode->Meshes));
				}
			}
			m_objectCount = objectIndex;
		}

		void SkinSkeletalMeshes() {
			auto prepared = false;
			for (const auto& renderObject : RenderObjects) {
				if (!renderObject.IsVisible) continue;

				const auto& model = renderObject.Model;
				if (const auto& animationCollection = renderObject.AnimationCollection; !empty(animationCollection) && animationCollection.HasBoneInfo()) {
					if (const auto& skeletalTransforms = animationCollection[animationCollection.GetSelectedIndex()].GetSkeletalTransforms(); !empty(skeletalTransforms)) {
						model.SkeletalTransforms->Upload(skeletalTransforms);
					}

					for (const auto& meshNode : model.MeshNodes) {
						for (const auto& mesh : meshNode->Meshes) {
							if (mesh->SkeletalVertices) {
								if (!prepared) {
									m_commandList.Begin();

									m_skeletalMeshSkinning.Prepare(m_commandList);

									prepared = true;
								}

								m_skeletalMeshSkinning.GPUBuffers = {
									.InSkeletalVertices = mesh->SkeletalVertices.get(),
									.InSkeletalTransforms = model.SkeletalTransforms.get(),
									.OutVertices = mesh->Vertices.get(),
									.OutMotionVectors = mesh->MotionVectors.get()
								};

								m_skeletalMeshSkinning.Process(m_commandList);
							}
						}
					}
				}
			}
			if (prepared) m_commandList.End(m_commandQueue).get();
		}

		const auto& GetTopLevelAccelerationStructure() const { return *m_topLevelAccelerationStructure; }

		void CreateAccelerationStructures(bool updateOnly) {
			vector<uint64_t> newBottomLevelAccelerationStructureIDs;

			{
				m_commandList.Begin();

				if (!updateOnly) {
					m_bottomLevelAccelerationStructureIDs = {};

					m_accelerationStructureManager = make_unique<DxAccelStructManager>(m_device);
					m_accelerationStructureManager->Initialize();
				}

				vector<vector<D3D12_RAYTRACING_GEOMETRY_DESC>> geometryDescs;
				vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> newBuildBottomLevelAccelerationStructureInputs, updatedBuildBottomLevelAccelerationStructureInputs;
				vector<const MeshNode*> newMeshNodes;
				vector<uint64_t> updatedBottomLevelAccelerationStructureIDs;

				for (const auto& renderObject : RenderObjects) {
					const auto& model = renderObject.Model;

					const auto& animationCollection = renderObject.AnimationCollection;
					const auto isAnimated = updateOnly && !empty(animationCollection) && animationCollection.HasBoneInfo();

					for (const auto& meshNode : model.MeshNodes) {
						auto isSkeletal = false;
						for (const auto& mesh : meshNode->Meshes) {
							if (mesh->SkeletalVertices) {
								isSkeletal = true;
								break;
							}
						}

						if (const auto [first, second] = m_bottomLevelAccelerationStructureIDs.try_emplace(meshNode.get()); second || (renderObject.IsVisible && isAnimated && isSkeletal)) {
							auto& _geometryDescs = geometryDescs.emplace_back();
							_geometryDescs.reserve(size(meshNode->Meshes));
							for (const auto& mesh : meshNode->Meshes) {
								_geometryDescs.emplace_back(CreateGeometryDesc(*mesh->Vertices, *mesh->Indices, mesh->MaterialIndex == ~0u || model.Materials[mesh->MaterialIndex].AlphaMode == AlphaMode::Opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE));
							}

							if (const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildBottomLevelAccelerationStructureInputs{
								.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
								.Flags = (isSkeletal ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | (isAnimated ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE) : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE) | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION,
								.NumDescs = static_cast<UINT>(size(_geometryDescs)),
								.pGeometryDescs = data(_geometryDescs)
								};
								second) {
								newBuildBottomLevelAccelerationStructureInputs.emplace_back(buildBottomLevelAccelerationStructureInputs);
								newMeshNodes.emplace_back(meshNode.get());
							}
							else {
								updatedBuildBottomLevelAccelerationStructureInputs.emplace_back(buildBottomLevelAccelerationStructureInputs);
								updatedBottomLevelAccelerationStructureIDs.emplace_back(first->second);
							}
						}
					}
				}

				if (!empty(newBuildBottomLevelAccelerationStructureInputs)) {
					m_accelerationStructureManager->PopulateBuildCommandList(m_commandList, data(newBuildBottomLevelAccelerationStructureInputs), size(newBuildBottomLevelAccelerationStructureInputs), newBottomLevelAccelerationStructureIDs);
					for (UINT i = 0; const auto & meshNode : newMeshNodes) m_bottomLevelAccelerationStructureIDs[meshNode] = newBottomLevelAccelerationStructureIDs[i++];
					m_accelerationStructureManager->PopulateUAVBarriersCommandList(m_commandList, newBottomLevelAccelerationStructureIDs);
					m_accelerationStructureManager->PopulateCompactionSizeCopiesCommandList(m_commandList, newBottomLevelAccelerationStructureIDs);
				}
				if (!empty(updatedBuildBottomLevelAccelerationStructureInputs)) {
					m_accelerationStructureManager->PopulateUpdateCommandList(m_commandList, data(updatedBuildBottomLevelAccelerationStructureInputs), size(updatedBuildBottomLevelAccelerationStructureInputs), updatedBottomLevelAccelerationStructureIDs);
					m_accelerationStructureManager->PopulateUAVBarriersCommandList(m_commandList, updatedBottomLevelAccelerationStructureIDs);
				}

				m_commandList.End(m_commandQueue).get();
			}

			{
				m_commandList.Begin();

				if (!empty(newBottomLevelAccelerationStructureIDs)) m_accelerationStructureManager->PopulateCompactionCommandList(m_commandList, newBottomLevelAccelerationStructureIDs);

				if (!updateOnly) {
					m_topLevelAccelerationStructure = make_unique<TopLevelAccelerationStructure>(m_device, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
				}
				vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
				instanceDescs.reserve(size(m_instanceData));
				for (UINT instanceIndex = 0; const auto & renderObject : RenderObjects) {
					for (const auto& meshNode : renderObject.Model.MeshNodes) {
						auto& instanceDesc = instanceDescs.emplace_back(D3D12_RAYTRACING_INSTANCE_DESC{
							.InstanceMask = renderObject.IsVisible ? ~0u : 0,
							.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE,
							.AccelerationStructure = m_accelerationStructureManager->GetAccelStructGPUVA(m_bottomLevelAccelerationStructureIDs.at(meshNode.get()))
							});
						reinterpret_cast<XMFLOAT3X4&>(instanceDesc.Transform) = m_instanceData[instanceIndex++].ObjectToWorld;
					}
				}
				m_topLevelAccelerationStructure->Build(m_commandList, instanceDescs, updateOnly);

				m_commandList.End(m_commandQueue).get();
			}

			if (!empty(newBottomLevelAccelerationStructureIDs)) m_accelerationStructureManager->GarbageCollection(newBottomLevelAccelerationStructureIDs);
		}

	private:
		ID3D12Device5* m_device;
		ID3D12CommandQueue* m_commandQueue;
		CommandList<ID3D12GraphicsCommandList4> m_commandList;

		SkeletalMeshSkinning m_skeletalMeshSkinning;

		vector<InstanceData> m_instanceData;
		UINT m_objectCount{};

		unique_ptr<DxAccelStructManager> m_accelerationStructureManager;
		unordered_map<const MeshNode*, uint64_t> m_bottomLevelAccelerationStructureIDs;
		unique_ptr<TopLevelAccelerationStructure> m_topLevelAccelerationStructure;

		struct Observer {
			Observer(Scene& scene) : m_scene(scene) {
				m_meshNodeDelete = MeshNode::DeleteEvent += [&](const MeshNode* ptr) {
					scene.m_bottomLevelAccelerationStructureIDs.erase(ptr);
				};
			}

			~Observer() { MeshNode::DeleteEvent -= m_meshNodeDelete; }

		private:
			Scene& m_scene;
			EventHandle m_meshNodeDelete{};
		} m_observer;
	};
}
