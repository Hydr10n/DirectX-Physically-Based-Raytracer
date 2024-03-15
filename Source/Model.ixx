module;

#include <filesystem>
#include <map>
#include <ranges>

#include "assimp/GltfMaterial.h"
#include "assimp/Importer.hpp"
#include "assimp/postprocess.h"
#include "assimp/scene.h"

#include "directxtk12/DirectXHelpers.h"
#include "directxtk12/SimpleMath.h"
#include "directxtk12/ResourceUploadBatch.h"

export module Model;

import CommandList;
import DescriptorHeap;
import ErrorHelpers;
import Event;
import GPUBuffer;
import Material;
import ResourceHelpers;
import Texture;
import Vertex;

using namespace Assimp;
using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace ResourceHelpers;
using namespace std;
using namespace std::filesystem;

namespace {
	constexpr auto ToBoundingBox(const aiAABB& AABB) {
		return BoundingBox{ reinterpret_cast<const XMFLOAT3&>((AABB.mMin + AABB.mMax) * 0.5f), reinterpret_cast<const XMFLOAT3&>((AABB.mMax - AABB.mMin) * 0.5f) };
	}
}

export {
	struct BoneInfo {
		UINT ID = ~0u;
		Matrix Transform;
	};

	using BoneInfoDictionary = unordered_map<string, BoneInfo>;

	struct Mesh {
		using VertexType = VertexPositionNormalTextureTangent;
		using IndexType = UINT32;
		using SkeletalVertexType = VertexPositionNormalTangentBones;

		string Name;

		shared_ptr<DefaultBuffer<VertexType>> Vertices;
		shared_ptr<DefaultBuffer<IndexType>> Indices;
		shared_ptr<DefaultBuffer<SkeletalVertexType>> SkeletalVertices;
		shared_ptr<DefaultBuffer<XMFLOAT3>> MotionVectors;

		struct { UINT Vertices = ~0u, Indices = ~0u, MotionVectors = ~0u; } DescriptorHeapIndices;

		bool HasTangents{};

		BoundingBox BoundingBox{ {}, {} };

		UINT MaterialIndex = ~0u;

		VertexDesc GetVertexDesc() const {
			return {
				.Stride = sizeof(VertexType),
				.NormalOffset = offsetof(VertexType, normal),
				.TextureCoordinateOffset = offsetof(VertexType, textureCoordinate),
				.TangentOffset = static_cast<UINT>(HasTangents ? offsetof(VertexType, tangent) : ~0u)
			};
		}
	};

	struct MeshNode {
		string Name;

		vector<shared_ptr<Mesh>> Meshes;

		BoundingBox BoundingBox{ {}, {} };

		Matrix GlobalTransform;

		inline static Event<const MeshNode*> DeleteEvent;

		~MeshNode() { DeleteEvent.Raise(this); }
	};

	struct Model {
		string Name;

		vector<shared_ptr<MeshNode>> MeshNodes;

		BoneInfoDictionary BoneInfo;

		shared_ptr<UploadBuffer<XMFLOAT3X4>> SkeletalTransforms;

		BoundingBox BoundingBox{ {}, {} };

		vector<Material> Materials;
		vector<map<TextureType, Texture>> Textures;

		Model() = default;

		Model(const Model& source, ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) {
			if (!source.SkeletalTransforms) {
				*this = source;

				return;
			}

			CommandList commandList(pDevice);
			commandList.Begin();

			MeshNodes.reserve(size(source.MeshNodes));
			for (const auto& meshNode : source.MeshNodes) {
				shared_ptr<MeshNode> newMeshNode;
				for (const auto& mesh : meshNode->Meshes) {
					if (mesh->SkeletalVertices) {
						newMeshNode = make_shared<MeshNode>();
						break;
					}
				}
				if (newMeshNode) {
					newMeshNode->Meshes.reserve(size(meshNode->Meshes));
					for (const auto& mesh : meshNode->Meshes) {
						if (mesh->SkeletalVertices) {
							const auto newMesh = make_shared<Mesh>();

							{
								const auto CopyBuffer = [&]<typename T>(shared_ptr<T>&destination, const shared_ptr<T>&source, UINT & SRVDescriptorHeapIndex, bool isVertex, bool copy) {
									destination = make_shared<T>(*source, copy ? commandList.GetNative() : nullptr);
									descriptorHeapIndex = descriptorHeap.Allocate(1, descriptorHeapIndex);
									SRVDescriptorHeapIndex = descriptorHeapIndex - 1;
									if (isVertex) destination->CreateRawSRV(descriptorHeap.GetCpuHandle(SRVDescriptorHeapIndex));
									else destination->CreateStructuredSRV(descriptorHeap.GetCpuHandle(SRVDescriptorHeapIndex));
								};
								CopyBuffer(newMesh->Vertices, mesh->Vertices, newMesh->DescriptorHeapIndices.Vertices, true, true);
								CopyBuffer(newMesh->MotionVectors, mesh->MotionVectors, newMesh->DescriptorHeapIndices.MotionVectors, false, false);
							}

							newMesh->Name = mesh->Name;
							newMesh->Indices = mesh->Indices;
							newMesh->SkeletalVertices = mesh->SkeletalVertices;
							newMesh->DescriptorHeapIndices.Indices = mesh->DescriptorHeapIndices.Indices;
							newMesh->HasTangents = mesh->HasTangents;
							newMesh->BoundingBox = mesh->BoundingBox;
							newMesh->MaterialIndex = mesh->MaterialIndex;

							newMeshNode->Meshes.emplace_back(newMesh);
						}
						else newMeshNode->Meshes.emplace_back(mesh);
					}

					newMeshNode->Name = meshNode->Name;
					newMeshNode->BoundingBox = meshNode->BoundingBox;
					newMeshNode->GlobalTransform = meshNode->GlobalTransform;

					MeshNodes.emplace_back(newMeshNode);
				}
				else MeshNodes.emplace_back(meshNode);
			}

			commandList.End(pCommandQueue).get();

			SkeletalTransforms = make_shared<UploadBuffer<XMFLOAT3X4>>(*source.SkeletalTransforms);

			Name = source.Name;
			BoneInfo = source.BoneInfo;
			BoundingBox = source.BoundingBox;
			Materials = source.Materials;
			Textures = source.Textures;
		}

		void Load(const path& filePath, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) {
			if (empty(filePath)) throw invalid_argument("Model file path cannot be empty");

			Importer importer;
			const auto scene = importer.ReadFile(reinterpret_cast<const char*>(filePath.u8string().c_str()), aiProcessPreset_TargetRealtime_MaxQuality | aiProcess_ConvertToLeftHanded);
			if (scene == nullptr || scene->mRootNode == nullptr) throw runtime_error(format("Assimp: {}", importer.GetErrorString()));

			Name = scene->mName.C_Str();

			constexpr auto InitializeAABB = [] { return aiAABB{ aiVector3D(numeric_limits<float>::max()), aiVector3D(-numeric_limits<float>::max()) }; };
			constexpr auto CalculateAABB = [](const aiAABB& AABB, aiAABB& newAABB) {
				newAABB.mMin.x = min(newAABB.mMin.x, AABB.mMin.x);
				newAABB.mMin.y = min(newAABB.mMin.y, AABB.mMin.y);
				newAABB.mMin.z = min(newAABB.mMin.z, AABB.mMin.z);
				newAABB.mMax.x = max(newAABB.mMax.x, AABB.mMax.x);
				newAABB.mMax.y = max(newAABB.mMax.y, AABB.mMax.y);
				newAABB.mMax.z = max(newAABB.mMax.z, AABB.mMax.z);
			};

			auto modelAABB = InitializeAABB();

			vector<LoadedTexture> loadedTextures;
			const auto ProcessNode = [&](this auto& self, aiNode& node) -> void {
				if (node.mParent != nullptr) node.mTransformation = node.mParent->mTransformation * node.mTransformation;

				if (node.mNumMeshes) {
					auto meshAABB = InitializeAABB();

					auto meshNode = make_shared<MeshNode>();

					meshNode->Name = node.mName.C_Str();

					meshNode->GlobalTransform = reinterpret_cast<const Matrix&>(node.mTransformation).Transpose();

					meshNode->Meshes.reserve(node.mNumMeshes);
					for (const auto i : views::iota(0u, node.mNumMeshes)) {
						auto& mesh = *scene->mMeshes[node.mMeshes[i]];
						if (const auto _mesh = ProcessMesh(filePath, *scene, node.mTransformation, mesh, loadedTextures, pDevice, resourceUploadBatch, descriptorHeap, descriptorHeapIndex)) {
							meshNode->Meshes.emplace_back(_mesh);

							CalculateAABB(mesh.mAABB, meshAABB);
						}
					}

					meshNode->BoundingBox = ToBoundingBox(meshAABB);

					MeshNodes.emplace_back(meshNode);

					CalculateAABB(meshAABB, modelAABB);
				}

				for (const auto i : views::iota(0u, node.mNumChildren)) self(*node.mChildren[i]);
			};
			ProcessNode(*scene->mRootNode);

			if (const auto count = size(BoneInfo)) SkeletalTransforms = make_shared<UploadBuffer<XMFLOAT3X4>>(pDevice, count);

			BoundingBox = ToBoundingBox(modelAABB);
		}

	private:
		struct LoadedTexture {
			bool IsEmbedded;
			path FilePath;
			Texture Resource;

			bool IsSameAs(const path& filePath, bool isEmbedded) const { return IsEmbedded == isEmbedded && (IsEmbedded ? FilePath == filePath : AreSamePath(FilePath, filePath)); }
		};

		shared_ptr<Mesh> ProcessMesh(const path& modelFilePath, const aiScene& scene, const aiMatrix4x4& transform, aiMesh& mesh, vector<LoadedTexture>& loadedTextures, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) {
			if (mesh.mNumVertices < 3) return nullptr;
			vector<Mesh::VertexType> vertices;
			vertices.reserve(mesh.mNumVertices);
			mesh.mAABB = { aiVector3D(numeric_limits<float>::max()), -aiVector3D(numeric_limits<float>::max()) };
			for (const auto i : views::iota(0u, mesh.mNumVertices)) {
				auto& vertex = vertices.emplace_back();
				if (mesh.HasPositions()) {
					auto position = mesh.mVertices[i];
					vertex.position = reinterpret_cast<const XMFLOAT3&>(position);
					position = transform * position;
					if (position.x < mesh.mAABB.mMin.x) mesh.mAABB.mMin.x = position.x;
					else if (position.x > mesh.mAABB.mMax.x) mesh.mAABB.mMax.x = position.x;
					if (position.y < mesh.mAABB.mMin.y) mesh.mAABB.mMin.y = position.y;
					else if (position.y > mesh.mAABB.mMax.y) mesh.mAABB.mMax.y = position.y;
					if (position.z < mesh.mAABB.mMin.z) mesh.mAABB.mMin.z = position.z;
					else if (position.z > mesh.mAABB.mMax.z) mesh.mAABB.mMax.z = position.z;
				}
				if (mesh.HasNormals()) vertex.normal = reinterpret_cast<const XMFLOAT3&>(mesh.mNormals[i]);
				if (mesh.HasTextureCoords(0)) vertex.textureCoordinate = reinterpret_cast<const XMFLOAT2&>(mesh.mTextureCoords[0][i]);
				if (mesh.HasTangentsAndBitangents()) vertex.tangent = reinterpret_cast<const XMFLOAT3&>(mesh.mTangents[i]);
			}

			uint32_t indexCount = 0;
			for (const auto i : views::iota(0u, mesh.mNumFaces)) indexCount += mesh.mFaces[i].mNumIndices;
			if (indexCount % 3 != 0) return nullptr;
			vector<Mesh::IndexType> indices;
			indices.reserve(indexCount);
			for (const auto i : views::iota(0u, mesh.mNumFaces)) for (const auto j : views::iota(0u, mesh.mFaces[i].mNumIndices)) indices.emplace_back(mesh.mFaces[i].mIndices[j]);

			const auto _mesh = make_shared<Mesh>();

			_mesh->Name = mesh.mName.C_Str();

			{
				const auto CreateBuffer = [&]<typename T>(shared_ptr<T>&buffer, const auto & data, D3D12_RESOURCE_STATES afterState, UINT * pSRVDescriptorHeapIndex, bool isVertex) {
					buffer = make_shared<T>(pDevice, resourceUploadBatch, data, afterState);
					if (pSRVDescriptorHeapIndex != nullptr) {
						descriptorHeapIndex = descriptorHeap.Allocate(1, descriptorHeapIndex);
						*pSRVDescriptorHeapIndex = descriptorHeapIndex - 1;
						if (isVertex) buffer->CreateRawSRV(descriptorHeap.GetCpuHandle(*pSRVDescriptorHeapIndex));
						else buffer->CreateStructuredSRV(descriptorHeap.GetCpuHandle(*pSRVDescriptorHeapIndex));
					}
				};

				CreateBuffer(_mesh->Vertices, vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &_mesh->DescriptorHeapIndices.Vertices, true);
				CreateBuffer(_mesh->Indices, indices, D3D12_RESOURCE_STATE_INDEX_BUFFER, &_mesh->DescriptorHeapIndices.Indices, false);

				if (mesh.HasBones()) {
					CreateBuffer(_mesh->SkeletalVertices, GetBones(vertices, mesh), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr, false);
					CreateBuffer(_mesh->MotionVectors, vector<XMFLOAT3>(size(vertices)), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, &_mesh->DescriptorHeapIndices.MotionVectors, true);
				}
			}

			_mesh->HasTangents = mesh.HasTangentsAndBitangents();

			_mesh->BoundingBox = ToBoundingBox(mesh.mAABB);

			if (scene.HasMaterials()) {
				_mesh->MaterialIndex = static_cast<UINT>(size(Materials));

				const auto& material = *scene.mMaterials[mesh.mMaterialIndex];

				auto& _material = Materials.emplace_back();
				aiColor4D color;
				if (material.Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS || material.Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
					_material.BaseColor = reinterpret_cast<const XMFLOAT4&>(color);
				}
				if (material.Get(AI_MATKEY_COLOR_EMISSIVE, color) == AI_SUCCESS) _material.EmissiveColor = reinterpret_cast<const XMFLOAT3&>(color);
				material.Get(AI_MATKEY_METALLIC_FACTOR, _material.Metallic);
				material.Get(AI_MATKEY_ROUGHNESS_FACTOR, _material.Roughness);
				if (material.Get(AI_MATKEY_TRANSMISSION_FACTOR, _material.Opacity) == AI_SUCCESS) _material.Opacity = 1 - _material.Opacity;
				else material.Get(AI_MATKEY_OPACITY, _material.Opacity);
				material.Get(AI_MATKEY_REFRACTI, _material.RefractiveIndex);
				if (aiString alphaMode; material.Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == aiReturn_SUCCESS) {
					if (!_stricmp(alphaMode.C_Str(), "Opaque")) _material.AlphaMode = AlphaMode::Opaque;
					else if (!_stricmp(alphaMode.C_Str(), "Blend")) _material.AlphaMode = AlphaMode::Blend;
					else if (!_stricmp(alphaMode.C_Str(), "Mask")) _material.AlphaMode = AlphaMode::Mask;
					material.Get(AI_MATKEY_GLTF_ALPHACUTOFF, _material.AlphaThreshold);
				}

				auto& textures = Textures.emplace_back();
				for (const auto textureType : {
					TextureType::BaseColorMap,
					TextureType::EmissiveColorMap,
					TextureType::MetallicMap,
					TextureType::RoughnessMap,
					TextureType::AmbientOcclusionMap,
					TextureType::TransmissionMap,
					TextureType::OpacityMap,
					TextureType::NormalMap
					}) {
					aiTextureType type;
					switch (textureType) {
						case TextureType::BaseColorMap: type = aiTextureType_BASE_COLOR; break;
						case TextureType::EmissiveColorMap: type = aiTextureType_EMISSION_COLOR; break;
						case TextureType::MetallicMap: type = aiTextureType_METALNESS; break;
						case TextureType::RoughnessMap: type = aiTextureType_DIFFUSE_ROUGHNESS; break;
						case TextureType::AmbientOcclusionMap: type = aiTextureType_AMBIENT_OCCLUSION; break;
						case TextureType::TransmissionMap: type = aiTextureType_TRANSMISSION; break;
						case TextureType::OpacityMap: type = aiTextureType_OPACITY; break;
						case TextureType::NormalMap: type = aiTextureType_NORMALS; break;
						default: throw;
					}
					if (aiString filePath;
						material.GetTexture(type, 0, &filePath) == AI_SUCCESS
						|| (textureType == TextureType::BaseColorMap && material.GetTexture(aiTextureType_DIFFUSE, 0, &filePath) == AI_SUCCESS)
						|| (textureType == TextureType::EmissiveColorMap && material.GetTexture(aiTextureType_EMISSIVE, 0, &filePath) == AI_SUCCESS)
						|| (textureType == TextureType::NormalMap
							&& (material.GetTexture(aiTextureType_HEIGHT, 0, &filePath) == AI_SUCCESS || material.GetTexture(aiTextureType_NORMAL_CAMERA, 0, &filePath) == AI_SUCCESS))) {
						path textureFilePath = reinterpret_cast<const char8_t*>(filePath.C_Str());
						const auto embeddedTexture = scene.GetEmbeddedTexture(filePath.C_Str());
						const auto isEmbedded = embeddedTexture != nullptr;
						if (!isEmbedded) textureFilePath = path(modelFilePath).replace_filename(textureFilePath);
						auto& texture = textures[textureType];
						if (const auto pLoadedTexture = ranges::find_if(loadedTextures, [&](const auto& value) { return value.IsSameAs(textureFilePath, isEmbedded); });
							pLoadedTexture == cend(loadedTextures)) {
							texture.Name = material.GetName().C_Str();

							if (isEmbedded) {
								texture.Load(embeddedTexture->achFormatHint, embeddedTexture->pcData, embeddedTexture->mHeight ? embeddedTexture->mWidth * embeddedTexture->mHeight * 4 : embeddedTexture->mWidth, pDevice, resourceUploadBatch, descriptorHeap, descriptorHeapIndex);
							}
							else texture.Load(textureFilePath, pDevice, resourceUploadBatch, descriptorHeap, descriptorHeapIndex);

							loadedTextures.emplace_back(isEmbedded, textureFilePath, texture);
						}
						else texture = pLoadedTexture->Resource;
					}
				}
			}

			return _mesh;
		}

		vector<Mesh::SkeletalVertexType> GetBones(const vector<Mesh::VertexType>& vertices, const aiMesh& mesh) {
			vector<Mesh::SkeletalVertexType> skeletalVertices;
			skeletalVertices.reserve(size(vertices));
			for (const auto& vertex : vertices) {
				auto& skeletalVertex = skeletalVertices.emplace_back();
				skeletalVertex.position = vertex.position;
				skeletalVertex.normal = vertex.normal;
				skeletalVertex.tangent = vertex.tangent;
			}

			for (const auto i : views::iota(0u, mesh.mNumBones)) {
				const auto& bone = *mesh.mBones[i];

				UINT boneID;
				const auto boneName = bone.mName.C_Str();
				if (const auto pBoneInfo = BoneInfo.find(boneName); pBoneInfo == cend(BoneInfo)) {
					const ::BoneInfo boneInfo{
						.ID = static_cast<UINT>(size(BoneInfo)),
						.Transform = reinterpret_cast<const Matrix&>(bone.mOffsetMatrix).Transpose()
					};
					BoneInfo[boneName] = boneInfo;
					boneID = boneInfo.ID;
				}
				else boneID = BoneInfo.at(boneName).ID;

				for (const auto i : views::iota(0u, bone.mNumWeights)) {
					const auto& weight = bone.mWeights[i];
					for (auto& vertex = skeletalVertices[weight.mVertexId]; const auto i : views::iota(0, 4)) {
						if (weight.mWeight != 0 && vertex.bones[i].ID == ~0u) {
							vertex.bones[i].ID = boneID;
							vertex.bones[i].Weight = weight.mWeight;
							break;
						}
					}
				}
			}

			return skeletalVertices;
		}
	};
}

struct ModelDictionaryLoader {
	void operator()(Model& resource, const path& filePath, ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) const {
		ResourceUploadBatch resourceUploadBatch(pDevice);
		resourceUploadBatch.Begin();

		resource.Load(filePath, pDevice, resourceUploadBatch, descriptorHeap, descriptorHeapIndex);

		resourceUploadBatch.End(pCommandQueue).get();
	}
};

export using ModelDictionary = ResourceDictionary<string, Model, ModelDictionaryLoader>;
