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

#include "MathLib.h"

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
using namespace Packed;
using namespace ResourceHelpers;
using namespace std;
using namespace std::filesystem;
using namespace TextureHelpers;

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

		BoundingBox BoundingBox{ {}, {} };

		vector<Material> Materials;
		vector<map<TextureMap, shared_ptr<Texture>>> Textures;

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
			BoundingBox = source.BoundingBox;
			Materials = source.Materials;
			Textures = source.Textures;
		}

		void Load(const path& filePath, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorIndex) {
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
						if (const auto _mesh = ProcessMesh(filePath, *scene, node.mTransformation, mesh, loadedTextures, pDevice, resourceUploadBatch, descriptorHeap, descriptorIndex)) {
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
			shared_ptr<Texture> Resource;

			bool IsSameAs(bool isEmbedded, const path& filePath) const { return IsEmbedded == isEmbedded && (IsEmbedded ? FilePath == filePath : AreSamePath(FilePath, filePath)); }
		};

		shared_ptr<Mesh> ProcessMesh(const path& modelFilePath, const aiScene& scene, const aiMatrix4x4& transform, aiMesh& mesh, vector<LoadedTexture>& loadedTextures, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorIndex) {
			if (mesh.mNumVertices < 3) return nullptr;
			vector<Mesh::VertexType> vertices;
			vertices.reserve(mesh.mNumVertices);
			mesh.mAABB = { aiVector3D(numeric_limits<float>::max()), -aiVector3D(numeric_limits<float>::max()) };
			for (const auto i : views::iota(0u, mesh.mNumVertices)) {
				auto& vertex = vertices.emplace_back();
				if (mesh.HasPositions()) {
					auto position = mesh.mVertices[i];
					vertex.Position = reinterpret_cast<const XMFLOAT3&>(position);
					position = transform * position;
					if (position.x < mesh.mAABB.mMin.x) mesh.mAABB.mMin.x = position.x;
					else if (position.x > mesh.mAABB.mMax.x) mesh.mAABB.mMax.x = position.x;
					if (position.y < mesh.mAABB.mMin.y) mesh.mAABB.mMin.y = position.y;
					else if (position.y > mesh.mAABB.mMax.y) mesh.mAABB.mMax.y = position.y;
					if (position.z < mesh.mAABB.mMin.z) mesh.mAABB.mMin.z = position.z;
					else if (position.z > mesh.mAABB.mMax.z) mesh.mAABB.mMax.z = position.z;
				}
				if (mesh.HasNormals()) {
					vertex.Normal = reinterpret_cast<const XMFLOAT2&>(EncodeUnitVector(reinterpret_cast<const float3&>(mesh.mNormals[i]), true));
				}
				if (mesh.HasTextureCoords(0)) {
					const auto& [x, y, z] = mesh.mTextureCoords[0][i];
					vertex.TextureCoordinate = sf2_to_h2(x, y);
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
					TextureMap::BaseColor,
					TextureMap::EmissiveColor,
					TextureMap::Metallic,
					TextureMap::Roughness,
					TextureMap::AmbientOcclusion,
					TextureMap::Transmission,
					TextureMap::Opacity,
					TextureMap::Normal
					}) {
					aiTextureType type;
					switch (textureType) {
						case TextureMap::BaseColor: type = aiTextureType_BASE_COLOR; break;
						case TextureMap::EmissiveColor: type = aiTextureType_EMISSION_COLOR; break;
						case TextureMap::Metallic: type = aiTextureType_METALNESS; break;
						case TextureMap::Roughness: type = aiTextureType_DIFFUSE_ROUGHNESS; break;
						case TextureMap::AmbientOcclusion: type = aiTextureType_AMBIENT_OCCLUSION; break;
						case TextureMap::Transmission: type = aiTextureType_TRANSMISSION; break;
						case TextureMap::Opacity: type = aiTextureType_OPACITY; break;
						case TextureMap::Normal: type = aiTextureType_NORMALS; break;
						default: throw;
					}
					if (aiString filePath;
						material.GetTexture(type, 0, &filePath) == AI_SUCCESS
						|| (textureType == TextureMap::BaseColor && material.GetTexture(aiTextureType_DIFFUSE, 0, &filePath) == AI_SUCCESS)
						|| (textureType == TextureMap::EmissiveColor && material.GetTexture(aiTextureType_EMISSIVE, 0, &filePath) == AI_SUCCESS)
						|| (textureType == TextureMap::Normal
							&& (material.GetTexture(aiTextureType_HEIGHT, 0, &filePath) == AI_SUCCESS || material.GetTexture(aiTextureType_NORMAL_CAMERA, 0, &filePath) == AI_SUCCESS))) {
						path textureFilePath = reinterpret_cast<const char8_t*>(filePath.C_Str());
						const auto embeddedTexture = scene.GetEmbeddedTexture(filePath.C_Str());
						const auto isEmbedded = embeddedTexture != nullptr;
						if (!isEmbedded) textureFilePath = path(modelFilePath).replace_filename(textureFilePath);
						auto& texture = textures[textureType];
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
