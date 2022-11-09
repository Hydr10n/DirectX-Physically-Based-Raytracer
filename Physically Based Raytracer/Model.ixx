module;

#include "pch.h"

#include "assimp/scene.h"
#include "assimp/Importer.hpp"
#include "assimp/postprocess.h"

#include "directxtk12/VertexTypes.h"

#include "directxtk12/DirectXHelpers.h"

#include "directxtk12/ResourceUploadBatch.h"

#include "directxtk12/DDSTextureLoader.h"
#include "directxtk12/WICTextureLoader.h"

#include <set>

#include <semaphore>

#include <filesystem>

export module Model;

import DirectX.DescriptorHeapEx;
import DirectX.RaytracingHelpers;
import Material;
import Texture;

using namespace Assimp;
using namespace DirectX;
using namespace DirectX::RaytracingHelpers;
using namespace DX;
using namespace std;
using namespace std::filesystem;

namespace {
	static void LoadTextureFromMemory(const aiTexture& texture, ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, D3D12_CPU_DESCRIPTOR_HANDLE descriptor, ID3D12Resource** ppTexture, bool* pIsCubeMap = nullptr) {
		bool isCubeMap = false;
		if (const auto size = texture.mHeight ? texture.mWidth * texture.mHeight * 4 : texture.mWidth; texture.CheckFormat("dds")) {
			ThrowIfFailed(CreateDDSTextureFromMemory(pDevice, resourceUploadBatch, reinterpret_cast<uint8_t*>(texture.pcData), size, ppTexture, false, 0, nullptr, &isCubeMap));
		}
		else ThrowIfFailed(CreateWICTextureFromMemoryEx(pDevice, resourceUploadBatch, reinterpret_cast<uint8_t*>(texture.pcData), size, 0, D3D12_RESOURCE_FLAG_NONE, WIC_LOADER_FORCE_RGBA32, ppTexture));
		CreateShaderResourceView(pDevice, *ppTexture, descriptor, isCubeMap);
		if (pIsCubeMap != nullptr) *pIsCubeMap = isCubeMap;
	}
}

export {
	struct VertexPositionNormalTextureTangent : VertexPositionNormalTexture { XMFLOAT3 tangent; };

	struct ModelMesh : TriangleMesh<VertexPositionNormalTextureTangent, UINT32> {
		using TriangleMesh<VertexPositionNormalTextureTangent, UINT32>::TriangleMesh;

		struct { UINT Vertices = ~0u, Indices = ~0u; } DescriptorHeapIndices;

		XMMATRIX Transform = XMMatrixIdentity();

		bool HasPerVertexTangents{};

		Material Material;

		TextureDictionary Textures;
	};

	struct Model {
		set<shared_ptr<ModelMesh>> Meshes;

		BoundingBox BoundingBox = DirectX::BoundingBox({}, {});

		path FilePath;

		struct LoaderFlags { enum { None, AdjustCenter = 0x1, RightHanded = 0x2 }; };
		UINT LoaderFlags{};

		void Load(ID3D12Device5* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex, const path& directoryPath = "") {
			const auto filePath = directoryPath / FilePath;

			Importer importer;
			const auto scene = importer.ReadFile(filePath.string(), aiProcessPreset_TargetRealtime_MaxQuality | aiProcess_FlipUVs | aiProcess_PreTransformVertices | aiProcess_GenBoundingBoxes | aiProcess_EmbedTextures | (LoaderFlags & LoaderFlags::RightHanded ? 0 : aiProcess_ConvertToLeftHanded));
			if (scene == nullptr || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || scene->mRootNode == nullptr) throw runtime_error(format("Assimp: {}", importer.GetErrorString()));

			aiVector3D minAABB(FLT_MAX), maxAABB(-FLT_MAX);
			for (auto i : views::iota(0u, scene->mNumMeshes)) {
				const auto& AABB = scene->mMeshes[i]->mAABB;
				minAABB.x = min(minAABB.x, AABB.mMin.x);
				minAABB.y = min(minAABB.y, AABB.mMin.y);
				minAABB.z = min(minAABB.z, AABB.mMin.z);
				maxAABB.x = max(maxAABB.x, AABB.mMax.x);
				maxAABB.y = max(maxAABB.y, AABB.mMax.y);
				maxAABB.z = max(maxAABB.z, AABB.mMax.z);
			}

			const auto extents = (maxAABB - minAABB) * 0.5f;
			const auto maxExtent = max({ BoundingBox.Extents.x, BoundingBox.Extents.y, BoundingBox.Extents.z }), scaling = maxExtent == 0 ? 1 : maxExtent / max({ extents.x, extents.y, extents.z });

			BoundingBox.Extents = { extents.x * scaling, extents.y * scaling, extents.z * scaling };

			aiMatrix4x4 transform;
			scene->mRootNode->mTransformation = aiMatrix4x4::Scaling(aiVector3D(scaling), transform);
			if (LoaderFlags & LoaderFlags::AdjustCenter) scene->mRootNode->mTransformation *= aiMatrix4x4::Translation({ extents.x - maxAABB.x, extents.y - maxAABB.y, extents.z - maxAABB.z }, transform);
			else BoundingBox.Center = { (maxAABB.x + minAABB.x) / 2 * scaling, (maxAABB.y + minAABB.y) / 2 * scaling, (maxAABB.z + minAABB.z) / 2 * scaling };

			TextureCollection loadedTextures;
			ProcessNode(*scene, *scene->mRootNode, filePath.parent_path(), loadedTextures, pDevice, resourceUploadBatch, descriptorHeap, descriptorHeapIndex);
		}

	private:
		using TextureCollection = vector<const Texture*>;

		void ProcessMesh(const aiScene& scene, const aiMesh& mesh, const path& directoryPath, TextureCollection& loadedTextures, ID3D12Device5* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) {
			const aiMatrix3x3 transform3x3(scene.mRootNode->mTransformation);
			vector<ModelMesh::VertexType> vertices;
			for (auto i : views::iota(0u, mesh.mNumVertices)) {
				ModelMesh::VertexType vertex;
				auto v = scene.mRootNode->mTransformation * mesh.mVertices[i];
				vertex.position = { v.x, v.y, v.z };
				if (mesh.HasNormals()) {
					v = (transform3x3 * mesh.mNormals[i]).Normalize();
					vertex.normal = { v.x, v.y, v.z };
				}
				if (mesh.HasTextureCoords(0)) vertex.textureCoordinate = { mesh.mTextureCoords[0][i].x, mesh.mTextureCoords[0][i].y };
				if (mesh.HasTangentsAndBitangents()) {
					v = (transform3x3 * mesh.mTangents[i]).Normalize();
					vertex.tangent = { v.x, v.y, v.z };
				}
				vertices.emplace_back(vertex);
			}
			if (vertices.size() < 3) return;

			vector<UINT32> indices;
			for (auto i : views::iota(0u, mesh.mNumFaces)) for (auto j : views::iota(0u, mesh.mFaces[i].mNumIndices)) indices.emplace_back(mesh.mFaces[i].mIndices[j]);
			if (indices.size() < 3) return;

			const auto modelMesh = make_shared<ModelMesh>(pDevice, vertices, indices);

			descriptorHeapIndex = descriptorHeap.Allocate(2, descriptorHeapIndex);
			modelMesh->DescriptorHeapIndices = {
				.Vertices = descriptorHeapIndex - 2,
				.Indices = descriptorHeapIndex - 1
			};
			modelMesh->CreateShaderResourceViews(descriptorHeap.GetCpuHandle(modelMesh->DescriptorHeapIndices.Vertices), descriptorHeap.GetCpuHandle(modelMesh->DescriptorHeapIndices.Indices));

			modelMesh->HasPerVertexTangents = mesh.HasTangentsAndBitangents();

			if (scene.HasMaterials()) {
				const auto& material = *scene.mMaterials[mesh.mMaterialIndex];

				aiColor4D color{};
				const auto ToXMFLOAT3 = [&] { return XMFLOAT3(color.r, color.g, color.b); };
				const auto ToXMFLOAT4 = [&] { return XMFLOAT4(color.r, color.g, color.b, color.a); };
				if (material.Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS || material.Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) modelMesh->Material.BaseColor = ToXMFLOAT4();
				if (material.Get(AI_MATKEY_COLOR_EMISSIVE, color) == AI_SUCCESS) modelMesh->Material.EmissiveColor = ToXMFLOAT4();
				material.Get(AI_MATKEY_ROUGHNESS_FACTOR, modelMesh->Material.Roughness);
				if (material.Get(AI_MATKEY_COLOR_SPECULAR, color) == AI_SUCCESS) modelMesh->Material.Specular = ToXMFLOAT3();
				material.Get(AI_MATKEY_METALLIC_FACTOR, modelMesh->Material.Metallic);
				material.Get(AI_MATKEY_REFRACTI, modelMesh->Material.RefractiveIndex);
				material.Get(AI_MATKEY_OPACITY, modelMesh->Material.Opacity);

				modelMesh->Textures.DirectoryPath = directoryPath;

				auto& textures = modelMesh->Textures[""];

				const auto ProcessTexture = [&](TextureType textureType) {
					aiTextureType type;
					switch (textureType) {
					case TextureType::BaseColorMap: type = aiTextureType_BASE_COLOR; break;
					case TextureType::EmissiveMap: type = aiTextureType_EMISSIVE; break;
					case TextureType::SpecularMap: type = aiTextureType_SPECULAR; break;
					case TextureType::MetallicMap: type = aiTextureType_METALNESS; break;
					case TextureType::RoughnessMap: type = aiTextureType_DIFFUSE_ROUGHNESS; break;
					case TextureType::AmbientOcclusionMap: type = aiTextureType_AMBIENT_OCCLUSION; break;
					case TextureType::OpacityMap: type = aiTextureType_OPACITY; break;
					case TextureType::NormalMap: type = aiTextureType_NORMALS; break;
					default: throw;
					}
					if (aiString path;
						material.GetTexture(type, 0, &path) == AI_SUCCESS
						|| (textureType == TextureType::BaseColorMap && material.GetTexture(aiTextureType_DIFFUSE, 0, &path) == AI_SUCCESS)
						|| (textureType == TextureType::NormalMap && material.GetTexture(aiTextureType_HEIGHT, 0, &path) == AI_SUCCESS)) {
						const Texture* pLoadedTexture = nullptr;
						for (const auto& loadedTexture : loadedTextures) {
							if (loadedTexture->FilePath == path.C_Str()) {
								pLoadedTexture = loadedTexture;
								break;
							}
						}
						if (auto& texture = get<0>(textures)[textureType]; pLoadedTexture == nullptr) {
							descriptorHeapIndex = descriptorHeap.Allocate(1, descriptorHeapIndex);
							texture.DescriptorHeapIndices.SRV = descriptorHeapIndex - 1;
							texture.FilePath = path.C_Str();

							bool isCubeMap;
							if (const auto embeddedTexture = scene.GetEmbeddedTexture(path.C_Str()); embeddedTexture != nullptr) {
								LoadTextureFromMemory(*embeddedTexture, pDevice, resourceUploadBatch, descriptorHeap.GetCpuHandle(texture.DescriptorHeapIndices.SRV), &texture.Resource, &isCubeMap);
							}
							else texture.Load(pDevice, resourceUploadBatch, descriptorHeap, &isCubeMap, directoryPath);
							if (isCubeMap) throw runtime_error(format("{}: Invalid texture", (directoryPath / texture.FilePath).string()));

							loadedTextures.emplace_back(&texture);
						}
						else texture = *pLoadedTexture;
					}
				};

				ProcessTexture(TextureType::BaseColorMap);
				ProcessTexture(TextureType::EmissiveMap);
				ProcessTexture(TextureType::SpecularMap);
				ProcessTexture(TextureType::MetallicMap);
				ProcessTexture(TextureType::RoughnessMap);
				ProcessTexture(TextureType::AmbientOcclusionMap);
				ProcessTexture(TextureType::OpacityMap);
				ProcessTexture(TextureType::NormalMap);

				get<1>(textures) = XMMatrixIdentity();
			}

			Meshes.emplace(modelMesh);
		}

		void ProcessNode(const aiScene& scene, const aiNode& node, const path& directoryPath, TextureCollection& loadedTextures, ID3D12Device5* pDevice, ResourceUploadBatch& resourceUploadBatch, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex) {
			const auto ProcessNode = [&](this auto& self, const aiNode& node) -> void {
				for (unsigned int i = 0; i < node.mNumMeshes; i++) ProcessMesh(scene, *scene.mMeshes[node.mMeshes[i]], directoryPath, loadedTextures, pDevice, resourceUploadBatch, descriptorHeap, descriptorHeapIndex);
				for (unsigned int i = 0; i < node.mNumChildren; i++) self(*node.mChildren[i]);
			};
			ProcessNode(node);
		}
	};

	struct ModelDictionary : map<string, shared_ptr<Model>, less<>> {
		using map<key_type, mapped_type, key_compare>::map;

		path DirectoryPath;

		void Load(ID3D12Device5* pDevice, ID3D12CommandQueue* pCommandQueue, DescriptorHeapEx& descriptorHeap, _Inout_ UINT& descriptorHeapIndex, UINT threadCount = 1) {
			if (!threadCount) throw invalid_argument("Thread count cannot be 0");

			exception_ptr exception;

			vector<thread> threads;

			vector<pair<size_t, const shared_ptr<Model>*>> loadedModels;

			mutex mutex;
			vector<unique_ptr<binary_semaphore>> semaphores;
			for (auto i : views::iota(0u, threadCount)) semaphores.emplace_back(make_unique<binary_semaphore>(0));

			for (auto& model : *this | views::values) {
				threads.emplace_back(
					[&](size_t threadIndex) {
						try {
							{
								const scoped_lock lock(mutex);

								const decltype(loadedModels)::value_type* pLoadedModel = nullptr;
								for (const auto& loadedModel : loadedModels) {
									if (loadedModel.second->get()->FilePath == model->FilePath) {
										pLoadedModel = &loadedModel;
										break;
									}
								}
								if (pLoadedModel != nullptr) {
									semaphores[pLoadedModel->first]->acquire();

									model = *pLoadedModel->second;

									semaphores[pLoadedModel->first]->release();

									return;
								}

								loadedModels.emplace_back(pair{ threadIndex, &model });
							}

							ResourceUploadBatch resourceUploadBatch(pDevice);
							resourceUploadBatch.Begin();

							model->Load(pDevice, resourceUploadBatch, descriptorHeap, descriptorHeapIndex, DirectoryPath);

							resourceUploadBatch.End(pCommandQueue).wait();

							semaphores[threadIndex]->release();
						}
						catch (...) { if (!exception) exception = current_exception(); }
					},
					threads.size()
						);

				if (std::size(threads) == threadCount) {
					for (auto& thread : threads) thread.join();
					threads.clear();
				}
			}

			if (!threads.empty()) for (auto& thread : threads) thread.join();

			if (exception) rethrow_exception(exception);
		}
	};
}
