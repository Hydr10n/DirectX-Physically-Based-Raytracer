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

#include "ml.h"
#include "ml.hlsli"

export module Model;

import CommandList;
import DescriptorHeap;
import ErrorHelpers;
import Event;
import GPUBuffer;
import Material;
import ResourceHelpers;
import TextureHelpers;
import Vertex;

using namespace Assimp;
using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace ErrorHelpers;
using namespace Microsoft::WRL;
using namespace Packing;
using namespace ResourceHelpers;
using namespace std;
using namespace std::filesystem;
using namespace TextureHelpers;

namespace {
	aiAABB InitializeAABB() { return { aiVector3D(numeric_limits<float>::max()), aiVector3D(-numeric_limits<float>::max()) }; }

	constexpr BoundingBox ToBoundingBox(const aiAABB& AABB) {
		return { reinterpret_cast<const XMFLOAT3&>((AABB.mMin + AABB.mMax) * 0.5f), reinterpret_cast<const XMFLOAT3&>((AABB.mMax - AABB.mMin) * 0.5f) };
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

		bool HasTangents{};

		BoundingBox BoundingBox{ {}, {} };

		UINT MaterialIndex = ~0u;

		VertexDesc GetVertexDesc() const {
			return {
				.Stride = sizeof(VertexType),
				.NormalOffset = offsetof(VertexType, Normal),
				.TextureCoordinateOffset = offsetof(VertexType, TextureCoordinate),
				.TangentOffset = static_cast<UINT>(HasTangents ? offsetof(VertexType, Tangent) : ~0u)
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

		vector<Material> Materials;
		vector<map<TextureMapType, shared_ptr<Texture>>> Textures;

		Model() = default;

		Model(const Model& source, ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorIndex) {
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
								const auto CopyBuffer = [&]<typename T>(shared_ptr<T>&destination, const shared_ptr<T>&source, bool isVertex) {
									destination = make_shared<T>(*source, isVertex ? commandList.GetNative() : nullptr);
									descriptorIndex = descriptorHeap.Allocate(1, descriptorIndex);
									if (isVertex) destination->CreateRawSRV(descriptorHeap, descriptorIndex - 1);
									else destination->CreateStructuredSRV(descriptorHeap, descriptorIndex - 1);
								};
								CopyBuffer(newMesh->Vertices, mesh->Vertices, true);
								CopyBuffer(newMesh->MotionVectors, mesh->MotionVectors, false);
							}

							newMesh->Name = mesh->Name;
							newMesh->Indices = mesh->Indices;
							newMesh->SkeletalVertices = mesh->SkeletalVertices;
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
			Materials = source.Materials;
			Textures = source.Textures;
		}

		void Load(const path& filePath, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorIndex) {
			if (empty(filePath)) throw invalid_argument("Model file path cannot be empty");

			Importer importer;
			const auto scene = importer.ReadFile(reinterpret_cast<const char*>(filePath.u8string().c_str()), aiProcessPreset_TargetRealtime_MaxQuality | aiProcess_ConvertToLeftHanded);
			if (scene == nullptr || scene->mRootNode == nullptr) {
				throw runtime_error(format("Assimp: {}", importer.GetErrorString()));
			}

			Name = scene->mName.C_Str();

			auto modelAABB = InitializeAABB();

			vector<LoadedTexture> loadedTextures;
			const auto ProcessNode = [&](this auto& self, aiNode& node) -> void {
				if (node.mParent != nullptr) node.mTransformation = node.mParent->mTransformation * node.mTransformation;

				if (node.mNumMeshes) {
					constexpr auto MergeAABB = [](aiAABB& newAABB, const aiAABB& AABB) {
						newAABB.mMin.x = ::min(newAABB.mMin.x, AABB.mMin.x);
						newAABB.mMin.y = ::min(newAABB.mMin.y, AABB.mMin.y);
						newAABB.mMin.z = ::min(newAABB.mMin.z, AABB.mMin.z);
						newAABB.mMax.x = ::max(newAABB.mMax.x, AABB.mMax.x);
						newAABB.mMax.y = ::max(newAABB.mMax.y, AABB.mMax.y);
						newAABB.mMax.z = ::max(newAABB.mMax.z, AABB.mMax.z);
					};

					auto meshAABB = InitializeAABB();

					auto meshNode = make_shared<MeshNode>();

					meshNode->Name = node.mName.C_Str();

					meshNode->GlobalTransform = reinterpret_cast<const Matrix&>(node.mTransformation).Transpose();

					meshNode->Meshes.reserve(node.mNumMeshes);
					for (const auto i : views::iota(0u, node.mNumMeshes)) {
						auto& mesh = *scene->mMeshes[node.mMeshes[i]];
						if (const auto _mesh = ProcessMesh(filePath, *scene, mesh, loadedTextures, pDevice, resourceUploadBatch, descriptorHeap, descriptorIndex)) {
							meshNode->Meshes.emplace_back(_mesh);

							MergeAABB(meshAABB, mesh.mAABB);
						}
					}

					meshNode->BoundingBox = ToBoundingBox(meshAABB);

					MeshNodes.emplace_back(meshNode);

					MergeAABB(modelAABB, meshAABB);
				}

				for (const auto i : views::iota(0u, node.mNumChildren)) self(*node.mChildren[i]);
			};
			ProcessNode(*scene->mRootNode);

			if (const auto count = size(BoneInfo)) SkeletalTransforms = make_shared<UploadBuffer<XMFLOAT3X4>>(pDevice, count);
		}

	private:
		struct LoadedTexture {
			bool IsEmbedded;
			path FilePath;
			shared_ptr<Texture> Resource;

			bool IsSameAs(bool isEmbedded, const path& filePath) const {
				return IsEmbedded == isEmbedded && (IsEmbedded ? FilePath == filePath : AreSamePath(FilePath, filePath));
			}
		};

		shared_ptr<Mesh> ProcessMesh(const path& modelFilePath, const aiScene& scene, aiMesh& mesh, vector<LoadedTexture>& loadedTextures, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorIndex) {
			if (mesh.mNumVertices < 3) return nullptr;

			vector<Mesh::VertexType> vertices;
			vertices.reserve(mesh.mNumVertices);

			mesh.mAABB = InitializeAABB();

			for (const auto i : views::iota(0u, mesh.mNumVertices)) {
				auto& vertex = vertices.emplace_back();
				if (mesh.HasPositions()) {
					const auto& position = mesh.mVertices[i];

					vertex.Position = reinterpret_cast<const XMFLOAT3&>(position);

					auto& AABB = mesh.mAABB;
					AABB.mMin.x = ::min(AABB.mMin.x, position.x);
					AABB.mMin.y = ::min(AABB.mMin.y, position.y);
					AABB.mMin.z = ::min(AABB.mMin.z, position.z);
					AABB.mMax.x = ::max(AABB.mMax.x, position.x);
					AABB.mMax.y = ::max(AABB.mMax.y, position.y);
					AABB.mMax.z = ::max(AABB.mMax.z, position.z);
				}
				if (mesh.HasNormals()) {
					vertex.Normal = reinterpret_cast<const XMFLOAT2&>(EncodeUnitVector(reinterpret_cast<const float3&>(mesh.mNormals[i]), true));
				}
				if (mesh.HasTextureCoords(0)) {
					vertex.TextureCoordinate = reinterpret_cast<const uint32_t&>(float2_to_float16_t2(reinterpret_cast<const float2&>(mesh.mTextureCoords[0][i])));
				}
				if (mesh.HasTangentsAndBitangents()) {
					vertex.Tangent = reinterpret_cast<const XMFLOAT2&>(EncodeUnitVector(reinterpret_cast<const float3&>(mesh.mTangents[i]), true));
				}
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
				const auto CreateBuffer = [&]<typename T>(shared_ptr<T>&buffer, const auto & data, D3D12_RESOURCE_STATES afterState, bool isStructuredSRV = true, bool hasSRV = true) {
					buffer = make_shared<T>(pDevice, resourceUploadBatch, data, afterState);
					if (hasSRV) {
						descriptorIndex = descriptorHeap.Allocate(1, descriptorIndex);
						if (isStructuredSRV) buffer->CreateStructuredSRV(descriptorHeap, descriptorIndex - 1);
						else buffer->CreateRawSRV(descriptorHeap, descriptorIndex - 1);
					}
				};

				CreateBuffer(_mesh->Vertices, vertices, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false);
				CreateBuffer(_mesh->Indices, indices, D3D12_RESOURCE_STATE_INDEX_BUFFER);

				if (mesh.HasBones()) {
					CreateBuffer(_mesh->SkeletalVertices, GetBones(vertices, mesh), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false, false);
					CreateBuffer(_mesh->MotionVectors, vector<XMFLOAT3>(size(vertices)), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
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
				if (material.Get(AI_MATKEY_TRANSMISSION_FACTOR, _material.Transmission) != AI_SUCCESS
					&& material.Get(AI_MATKEY_OPACITY, _material.Transmission) == AI_SUCCESS) {
					_material.Transmission = 1 - _material.Transmission;
				}
				material.Get(AI_MATKEY_REFRACTI, _material.IOR);
				if (aiString alphaMode; material.Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == aiReturn_SUCCESS) {
					if (!_stricmp(alphaMode.C_Str(), "Opaque")) _material.AlphaMode = AlphaMode::Opaque;
					else if (!_stricmp(alphaMode.C_Str(), "Blend")) _material.AlphaMode = AlphaMode::Blend;
					else if (!_stricmp(alphaMode.C_Str(), "Mask")) _material.AlphaMode = AlphaMode::Mask;
					material.Get(AI_MATKEY_GLTF_ALPHACUTOFF, _material.AlphaThreshold);
				}

				auto& textures = Textures.emplace_back();
				for (const auto textureMapType : {
					TextureMapType::BaseColor,
					TextureMapType::EmissiveColor,
					TextureMapType::Metallic,
					TextureMapType::Roughness,
					TextureMapType::AmbientOcclusion,
					TextureMapType::Transmission,
					TextureMapType::Opacity,
					TextureMapType::Normal
					}) {
					aiTextureType type;
					switch (textureMapType) {
						case TextureMapType::BaseColor: type = aiTextureType_BASE_COLOR; break;
						case TextureMapType::EmissiveColor: type = aiTextureType_EMISSION_COLOR; break;
						case TextureMapType::Metallic: type = aiTextureType_METALNESS; break;
						case TextureMapType::Roughness: type = aiTextureType_DIFFUSE_ROUGHNESS; break;
						case TextureMapType::AmbientOcclusion: type = aiTextureType_AMBIENT_OCCLUSION; break;
						case TextureMapType::Transmission: type = aiTextureType_TRANSMISSION; break;
						case TextureMapType::Opacity: type = aiTextureType_OPACITY; break;
						case TextureMapType::Normal: type = aiTextureType_NORMALS; break;
						default: throw;
					}
					if (aiString filePath;
						material.GetTexture(type, 0, &filePath) == AI_SUCCESS
						|| (textureMapType == TextureMapType::BaseColor && material.GetTexture(aiTextureType_DIFFUSE, 0, &filePath) == AI_SUCCESS)
						|| (textureMapType == TextureMapType::EmissiveColor && material.GetTexture(aiTextureType_EMISSIVE, 0, &filePath) == AI_SUCCESS)
						|| (textureMapType == TextureMapType::Normal
							&& (material.GetTexture(aiTextureType_HEIGHT, 0, &filePath) == AI_SUCCESS || material.GetTexture(aiTextureType_NORMAL_CAMERA, 0, &filePath) == AI_SUCCESS))) {
						path textureFilePath = reinterpret_cast<const char8_t*>(filePath.C_Str());
						const auto embeddedTexture = scene.GetEmbeddedTexture(filePath.C_Str());
						const auto isEmbedded = embeddedTexture != nullptr;
						if (!isEmbedded) textureFilePath = path(modelFilePath).replace_filename(textureFilePath);
						auto& texture = textures[textureMapType];
						if (const auto pLoadedTexture = ranges::find_if(loadedTextures, [&](const auto& value) { return value.IsSameAs(isEmbedded, textureFilePath); });
							pLoadedTexture == cend(loadedTextures)) {
							if (isEmbedded) {
								texture = LoadTexture(embeddedTexture->achFormatHint, embeddedTexture->pcData, embeddedTexture->mHeight ? embeddedTexture->mWidth * embeddedTexture->mHeight * 4 : embeddedTexture->mWidth, pDevice, resourceUploadBatch, descriptorHeap, descriptorIndex);
							}
							else {
								texture = LoadTexture(textureFilePath, pDevice, resourceUploadBatch, descriptorHeap, descriptorIndex);
							}

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
				skeletalVertex.Position = vertex.Position;
				skeletalVertex.Normal = vertex.Normal;
				skeletalVertex.Tangent = vertex.Tangent;
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
						if (weight.mWeight != 0 && vertex.Bones[i].ID == ~0u) {
							vertex.Bones[i].ID = boneID;
							vertex.Bones[i].Weight = weight.mWeight;
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
	void operator()(Model& resource, const path& filePath, ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorIndex) const {
		ResourceUploadBatch resourceUploadBatch(pDevice);
		resourceUploadBatch.Begin();

		resource.Load(filePath, pDevice, resourceUploadBatch, descriptorHeap, descriptorIndex);

		resourceUploadBatch.End(pCommandQueue).get();
	}
};

export using ModelDictionary = ResourceDictionary<string, Model, ModelDictionaryLoader>;
