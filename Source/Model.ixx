module;

#include <array>

#include <d3d12.h>

#include "directxtk12/SimpleMath.h"

#include "eventpp/callbacklist.h"

export module Model;

import CommandList;
import GPUBuffer;
import Material;
import Texture;
import Vertex;

using namespace DirectX;
using namespace DirectX::PackedVector;
using namespace DirectX::SimpleMath;
using namespace eventpp;
using namespace std;

export {
	struct Mesh {
		using VertexType = VertexPositionNormalTangentTexture;
		using SkeletalVertexType = VertexPositionNormalTangentSkin;
		using MotionVectorType = XMHALF4;
		shared_ptr<GPUBuffer> Vertices, Indices, SkeletalVertices, MotionVectors;

		bool HasNormals{}, HasTangents{}, HasTextureCoordinates[2]{};

		uint32_t MaterialIndex = ~0u, TextureIndex = ~0u;

		VertexDesc GetVertexDesc() const {
			return {
				.Stride = sizeof(VertexType),
				.AttributeOffsets{
					.Normal = HasNormals ? static_cast<uint32_t>(offsetof(VertexType, Normal)) : ~0u,
					.Tangent = HasTangents ? static_cast<uint32_t>(offsetof(VertexType, Tangent)) : ~0u,
					.TextureCoordinates{
						HasTextureCoordinates[0] ? static_cast<uint32_t>(offsetof(VertexType, TextureCoordinates[0])) : ~0u,
						HasTextureCoordinates[1] ? static_cast<uint32_t>(offsetof(VertexType, TextureCoordinates[1])) : ~0u
					}
				}
			};
		}
	};

	struct SkinJoint {
		string Name;
		Matrix InverseBindMatrix;
	};

	using SkinJointDictionary = unordered_map<string, shared_ptr<vector<SkinJoint>>>;

	struct MeshNode {
		string NodeName, MeshName;

		vector<shared_ptr<Mesh>> Meshes;

		Matrix GlobalTransform;

		shared_ptr<GPUBuffer> SkeletalTransforms;

		using DestroyEvent = CallbackList<void(MeshNode*)>;
		DestroyEvent OnDestroyed;

		~MeshNode() { OnDestroyed(this); }
	};

	struct Model {
		string Name;

		vector<shared_ptr<MeshNode>> MeshNodes;

		shared_ptr<SkinJointDictionary> SkinJoints;

		vector<Material> Materials;
		vector<array<tuple<shared_ptr<Texture>, uint32_t/*TextureCoordinateIndex*/>, TextureMapType::Count>> Textures;

		Model() = default;

		Model(const Model& source, CommandList& commandList) {
			if (!source.SkinJoints) {
				*this = source;

				return;
			}

			MeshNodes.reserve(size(source.MeshNodes));
			for (const auto& meshNode : source.MeshNodes) {
				if (meshNode->SkeletalTransforms) {
					const auto& deviceContext = commandList.GetDeviceContext();

					auto newMeshNode = make_shared<MeshNode>();
					newMeshNode->Meshes.reserve(size(meshNode->Meshes));
					for (const auto& mesh : meshNode->Meshes) {
						if (mesh->SkeletalVertices) {
							const auto newMesh = make_shared<Mesh>();

							{
								const auto CopyBuffer = [&]<typename T>(auto & destination, const auto & source, bool isVertex) {
									destination = GPUBuffer::CreateDefault<T>(deviceContext, source->GetCapacity());
									destination->CreateSRV(isVertex ? BufferSRVType::Raw : BufferSRVType::Structured);
									commandList.Copy(*destination, *source);
									commandList.SetState(*destination, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
								};
								CopyBuffer.operator() < Mesh::VertexType > (newMesh->Vertices, mesh->Vertices, true);
								CopyBuffer.operator() < Mesh::MotionVectorType > (newMesh->MotionVectors, mesh->MotionVectors, false);
							}

							newMesh->Indices = mesh->Indices;
							newMesh->SkeletalVertices = mesh->SkeletalVertices;
							newMesh->HasNormals = mesh->HasNormals;
							newMesh->HasTangents = mesh->HasTangents;
							ranges::copy(mesh->HasTextureCoordinates, newMesh->HasTextureCoordinates);
							newMesh->MaterialIndex = mesh->MaterialIndex;
							newMesh->TextureIndex = mesh->TextureIndex;

							newMeshNode->Meshes.emplace_back(newMesh);
						}
						else {
							newMeshNode->Meshes.emplace_back(mesh);
						}
					}

					newMeshNode->NodeName = meshNode->NodeName;
					newMeshNode->MeshName = meshNode->MeshName;

					newMeshNode->GlobalTransform = meshNode->GlobalTransform;

					newMeshNode->SkeletalTransforms = GPUBuffer::CreateDefault<XMFLOAT3X4>(deviceContext, meshNode->SkeletalTransforms->GetCapacity());

					MeshNodes.emplace_back(newMeshNode);
				}
				else {
					MeshNodes.emplace_back(meshNode);
				}
			}

			Name = source.Name;
			SkinJoints = source.SkinJoints;
			Materials = source.Materials;
			Textures = source.Textures;
		}
	};
}
