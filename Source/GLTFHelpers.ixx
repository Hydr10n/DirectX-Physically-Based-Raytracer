module;

#include <array>
#include <filesystem>
#include <ranges>

#include <d3d12.h>

#include "fastgltf/core.hpp"
#include "fastgltf/dxmath_element_traits.hpp"

#include "DirectXMesh.h"

#include "directxtk12/SimpleMath.h"

export module GLTFHelpers;

export import Animation;
import CommandList;
import DeviceContext;
import ErrorHelpers;
import GPUBuffer;
import Material;
export import Model;
import Math;
import ResourceHelpers;
import TextureHelpers;

using namespace DirectX;
using namespace DirectX::PackedVector;
using namespace DirectX::SimpleMath;
using namespace ErrorHelpers;
using namespace Math;
using namespace ResourceHelpers;
using namespace std;
using namespace std::filesystem;
using namespace TextureHelpers;

namespace {
	auto Load(
		const path& filePath,
		fastgltf::Category category = fastgltf::Category::All,
		fastgltf::Options options = fastgltf::Options::None,
		fastgltf::Extensions extensions = fastgltf::Extensions::None
	) {
		const auto ThrowIfFailed = [&](fastgltf::Error error) {
			if (error != fastgltf::Error::None) {
				throw runtime_error(format("{}: {}", filePath.string(), getErrorMessage(error)));
			}
		};
		auto data = fastgltf::GltfDataBuffer::FromPath(filePath);
		ThrowIfFailed(data.error());
		fastgltf::Parser parser(extensions);
		auto asset = parser.loadGltf(
			data.get(),
			filePath.parent_path(),
			options | fastgltf::Options::LoadExternalBuffers,
			category
		);
		ThrowIfFailed(asset.error());
		return *move(asset);
	}

	auto GetDefaultSceneIndex(const fastgltf::Asset& asset) {
		return clamp<size_t>(asset.defaultScene ? asset.defaultScene.value() : 0, 0, size(asset.scenes));
	}

	struct StoredSkinJoints {
		size_t Index;
		shared_ptr<vector<SkinJoint>> SkinJoints;
	};

	struct LoadedTexture {
		const void* Data;
		path FilePath;
		shared_ptr<Texture> Resource;

		bool IsSameAs(const path& filePath) const { return FilePath == filePath || AreSamePath(FilePath, filePath); }
	};

	void LoadTexture(
		const path& directoryPath, const fastgltf::Asset& asset, const fastgltf::TextureInfo& textureInfo,
		bool forceSRGB, shared_ptr<Texture>& texture, vector<LoadedTexture>& loadedTextures,
		CommandList& commandList
	) {
		size_t imageIndex;
		if (const auto& texture = asset.textures.at(textureInfo.textureIndex);
			texture.ddsImageIndex) {
			imageIndex = texture.ddsImageIndex.value();
		}
		else if (texture.imageIndex) {
			imageIndex = texture.imageIndex.value();
		}
		else {
			return;
		}

		const auto Load = [&](span<const std::byte> data, fastgltf::MimeType mimeType) {
			if (const auto pLoadedTexture = ranges::find_if(loadedTextures, [&](const auto& value) {
				return value.Data == ::data(data);
				});
				pLoadedTexture == cend(loadedTextures)) {
				const auto format = mimeType == fastgltf::MimeType::DDS ? "dds" : "";
				texture = ::LoadTexture(commandList, format, data, forceSRGB);
				texture->CreateSRV();

				loadedTextures.emplace_back(::data(data), "", texture);
			}
			else {
				texture = pLoadedTexture->Resource;
			}
		};
		const auto& image = asset.images.at(imageIndex);
		if (const auto view = get_if<fastgltf::sources::BufferView>(&image.data); view != nullptr) {
			const auto& bufferView = asset.bufferViews.at(view->bufferViewIndex);
			const auto& buffer = asset.buffers.at(bufferView.bufferIndex);
			if (const auto array = get_if<fastgltf::sources::Array>(&buffer.data); array != nullptr) {
				Load(span(data(array->bytes) + bufferView.byteOffset, bufferView.byteLength), array->mimeType);
			}
		}
		else if (const auto array = get_if<fastgltf::sources::Array>(&image.data); array != nullptr) {
			Load(array->bytes, array->mimeType);
		}
		else if (const auto URI = get_if<fastgltf::sources::URI>(&image.data);
			URI != nullptr && !URI->fileByteOffset && URI->uri.isLocalPath()) {
			const auto filePath = directoryPath / URI->uri.path();
			if (const auto pLoadedTexture = ranges::find_if(loadedTextures, [&](const auto& value) {
				return value.IsSameAs(filePath);
				});
				pLoadedTexture == cend(loadedTextures)) {
				texture = ::LoadTexture(commandList, filePath, forceSRGB);
				texture->CreateSRV();

				loadedTextures.emplace_back(nullptr, filePath, texture);
			}
			else {
				texture = pLoadedTexture->Resource;
			}
		}
	}

	shared_ptr<Mesh> ProcessPrimitive(
		const path& directoryPath,
		const fastgltf::Asset& asset, const fastgltf::Primitive& primitive,
		bool flipWindingOrder,
		Model& model,
		vector<LoadedTexture>& loadedTextures,
		CommandList& commandList
	) {
		if (primitive.type != fastgltf::PrimitiveType::Triangles) {
			return nullptr;
		}

		vector<Mesh::VertexType> vertices;
		if (const auto attribute = primitive.findAttribute("POSITION"); attribute != cend(primitive.attributes)) {
			const auto& accessor = asset.accessors.at(attribute->accessorIndex);
			vertices.reserve(accessor.count);
			fastgltf::iterateAccessor<XMFLOAT3>(
				asset, accessor,
				[&](const XMFLOAT3& value) {
					vertices.emplace_back(Mesh::VertexType{ .Position = value });
				}
			);
		}
		else {
			return nullptr;
		}

		vector<uint8_t> indices;
		size_t indexStride;
		if (primitive.indicesAccessor) {
			const auto& accessor = asset.accessors[primitive.indicesAccessor.value()];
			const auto Iterate = [&]<typename T> {
				indexStride = sizeof(T);
				indices.resize(accessor.count* indexStride);
				fastgltf::iterateAccessorWithIndex<T>(
					asset, accessor,
					[&](T value, size_t index) {
						reinterpret_cast<T*>(data(indices))[flipWindingOrder ? accessor.count - 1 - index : index] = value;
					}
				);
			};
			if (accessor.count <= UINT16_MAX) {
				Iterate.operator() < uint16_t > ();
			}
			else {
				Iterate.operator() < uint32_t > ();
			}
		}
		else {
			return nullptr;
		}

		const auto normalAttribute = primitive.findAttribute("NORMAL"), tangentAttribute = primitive.findAttribute("Tangent");

		vector<XMFLOAT2> textureCoordinates;
		bool hasTextureCoordinates[2]{};
		for (const auto i : { 0, 1 }) {
			if (const auto attribute = primitive.findAttribute(format("TEXCOORD_{}", i));
				attribute != cend(primitive.attributes)) {
				const auto shouldStoreTextureCoordinates = !i
					&& normalAttribute != cend(primitive.attributes) && tangentAttribute == cend(primitive.attributes);
				fastgltf::iterateAccessorWithIndex<XMFLOAT2>(
					asset, asset.accessors.at(attribute->accessorIndex),
					[&](const XMFLOAT2& value, size_t index) {
						vertices[index].StoreTextureCoordinate(value, i);

				if (shouldStoreTextureCoordinates) {
					if (empty(textureCoordinates)) {
						textureCoordinates.reserve(size(vertices));
					}
					textureCoordinates.emplace_back(value);
				}
					}
				);

				hasTextureCoordinates[i] = true;
			}
		}

		auto hasNormals = false, hasTangents = false;
		if (normalAttribute != cend(primitive.attributes)) {
			vector<XMFLOAT3> normals;
			const auto shouldStoreNormals = !empty(textureCoordinates) && tangentAttribute == cend(primitive.attributes);
			fastgltf::iterateAccessorWithIndex<XMFLOAT3>(
				asset, asset.accessors.at(normalAttribute->accessorIndex),
				[&](const XMFLOAT3& value, size_t index) {
					vertices[index].StoreNormal(value);

			if (shouldStoreNormals) {
				if (empty(normals)) {
					normals.reserve(size(vertices));
				}
				normals.emplace_back(value);
			}
				}
			);

			hasNormals = true;

			if (tangentAttribute != cend(primitive.attributes)) {
				fastgltf::iterateAccessorWithIndex<XMFLOAT4>(
					asset, asset.accessors.at(tangentAttribute->accessorIndex),
					[&](const XMFLOAT4& value, size_t index) {
						vertices[index].StoreTangent(reinterpret_cast<const XMFLOAT3&>(value));
					}
				);

				hasTangents = true;
			}
			else if (!empty(textureCoordinates)) {
				vector<XMFLOAT3> positions, tangents(size(vertices));
				positions.reserve(size(vertices));
				for (const auto& vertex : vertices) {
					positions.emplace_back(vertex.Position);
				}
				const auto ComputeTangentFrame = [&]<typename T> {
					ThrowIfFailed(::ComputeTangentFrame(
						reinterpret_cast<const T*>(data(indices)), size(indices) / 3 / indexStride,
						data(positions), data(normals), data(textureCoordinates), size(vertices),
						data(tangents), nullptr
					));
				};
				if (indexStride == sizeof(uint16_t)) {
					ComputeTangentFrame.operator() < uint16_t > ();
				}
				else {
					ComputeTangentFrame.operator() < uint32_t > ();
				}
				for (size_t i = 0; const auto & tangent : tangents) {
					vertices[i++].StoreTangent(tangent);
				}

				hasTangents = true;
			}
		}

		auto hasJoints = false;
		vector<Mesh::SkeletalVertexType> skeletalVertices;
		if (const auto attribute = primitive.findAttribute("JOINTS_0"); attribute != cend(primitive.attributes)) {
			skeletalVertices.reserve(size(vertices));
			fastgltf::iterateAccessor<fastgltf::math::u16vec4>(
				asset, asset.accessors.at(attribute->accessorIndex),
				[&](const fastgltf::math::u16vec4& value) {
					skeletalVertices.emplace_back(Mesh::SkeletalVertexType{ .Joints = reinterpret_cast<const XMUSHORT4&>(value) });
				}
			);

			if (const auto attribute = primitive.findAttribute("WEIGHTS_0"); attribute != cend(primitive.attributes)) {
				fastgltf::iterateAccessorWithIndex<XMFLOAT4>(
					asset, asset.accessors.at(attribute->accessorIndex),
					[&](const XMFLOAT4& value, size_t index) {
						auto& skeletalVertex = skeletalVertices[index];
				skeletalVertex.Weights = value;

				const auto& vertex = vertices[index];
				skeletalVertex.Position = vertex.Position;
				skeletalVertex.Normal = vertex.Normal;
				skeletalVertex.Tangent = vertex.Tangent;
					}
				);
				hasJoints = true;
			}
		}

		const auto mesh = make_shared<Mesh>();

		mesh->HasNormals = hasNormals;
		mesh->HasTangents = hasTangents;
		ranges::copy(hasTextureCoordinates, mesh->HasTextureCoordinates);

		const auto& deviceContext = commandList.GetDeviceContext();

		{
			const auto CreateBuffer = [&](auto& buffer, const auto& data, bool isStructuredSRV = true, bool hasSRV = true) {
				using T = typename remove_cvref_t<decltype(data)>::value_type;
				const auto format = &buffer == &mesh->Indices ?
					(indexStride == sizeof(uint16_t) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT) : DXGI_FORMAT_UNKNOWN;
				buffer = GPUBuffer::CreateDefault<T>(deviceContext, size(data), format);
				if (hasSRV) {
					buffer->CreateSRV(
						format == DXGI_FORMAT_UNKNOWN ?
						(isStructuredSRV ? BufferSRVType::Structured : BufferSRVType::Raw) : BufferSRVType::Typed
					);
				}
				if (::data(data) != nullptr) {
					commandList.Copy(*buffer, data);
				}
				commandList.SetState(*buffer, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			};

			CreateBuffer(mesh->Vertices, vertices, false);

			if (const auto size = ::size(indices) / indexStride;
				indexStride == sizeof(uint16_t)) {
				CreateBuffer(mesh->Indices, span(reinterpret_cast<const uint16_t*>(data(indices)), size));
			}
			else {
				CreateBuffer(mesh->Indices, span(reinterpret_cast<const uint32_t*>(data(indices)), size));
			}

			if (hasJoints) {
				CreateBuffer(mesh->SkeletalVertices, skeletalVertices, false, false);
				CreateBuffer(mesh->MotionVectors, span(static_cast<const Mesh::MotionVectorType*>(nullptr), size(vertices)));
			}
		}

		if (primitive.materialIndex) {
			mesh->MaterialIndex = static_cast<uint32_t>(size(model.Materials));

			const auto& material = asset.materials.at(primitive.materialIndex.value());

			auto& _material = model.Materials.emplace_back(Material{
				.BaseColor = reinterpret_cast<const XMFLOAT4&>(material.pbrData.baseColorFactor),
				.EmissiveStrength = material.emissiveStrength,
				.EmissiveColor = reinterpret_cast<const XMFLOAT3&>(material.emissiveFactor),
				.Metallic = material.pbrData.metallicFactor,
				.Roughness = material.pbrData.roughnessFactor,
				.IOR = material.ior,
				.AlphaMode = static_cast<AlphaMode>(material.alphaMode),
				.AlphaCutoff = material.alphaCutoff
				});
			if (material.transmission) {
				_material.Transmission = material.transmission->transmissionFactor;
			}

			if (hasTextureCoordinates[0] || hasTextureCoordinates[1]) {
				mesh->TextureIndex = static_cast<uint32_t>(size(model.Textures));

				for (auto& textures = model.Textures.emplace_back();
					const auto i : views::iota(0u, static_cast<uint32_t>(TextureMapType::Count))) {
					const fastgltf::TextureInfo* textureInfo = nullptr;
					auto forceSRGB = false;
					switch (i) {
						case TextureMapType::BaseColor:
						{
							if (material.pbrData.baseColorTexture) {
								textureInfo = &material.pbrData.baseColorTexture.value();
								forceSRGB = true;
							}
						}
						break;

						case TextureMapType::EmissiveColor:
						{
							if (material.emissiveTexture) {
								textureInfo = &material.emissiveTexture.value();
								forceSRGB = true;
							}
						}
						break;

						case TextureMapType::MetallicRoughness:
						{
							if (material.pbrData.metallicRoughnessTexture) {
								textureInfo = &material.pbrData.metallicRoughnessTexture.value();
							}
						}
						break;

						case TextureMapType::Transmission:
						{
							if (material.transmission && material.transmission->transmissionTexture) {
								textureInfo = &material.transmission->transmissionTexture.value();
							}
						}
						break;

						case TextureMapType::Normal:
						{
							if (hasTangents && material.normalTexture) {
								textureInfo = &material.normalTexture.value();
							}
						}
						break;
					}

					if (textureInfo != nullptr
						&& textureInfo->texCoordIndex < 2 && hasTextureCoordinates[textureInfo->texCoordIndex]) {
						auto& [Texture, TextureCoordinateIndex] = textures[i];
						LoadTexture(
							directoryPath, asset, *textureInfo,
							forceSRGB, Texture, loadedTextures,
							commandList
						);
						TextureCoordinateIndex = textureInfo->texCoordIndex;
					}
				}
			}
		}

		return mesh;
	}
}

export namespace GLTFHelpers {
	void LoadModel(
		Model& model,
		const path& filePath,
		CommandList& commandList,
		bool flipWindingOrder = false
	) {
		if (empty(filePath)) {
			throw invalid_argument("Model file path cannot be empty");
		}

		auto asset = ::Load(
			filePath,
			fastgltf::Category::OnlyRenderable | fastgltf::Category::Skins,
			fastgltf::Options::GenerateMeshIndices,
			fastgltf::Extensions::MSFT_texture_dds
			| fastgltf::Extensions::KHR_materials_emissive_strength
			| fastgltf::Extensions::KHR_materials_ior
			| fastgltf::Extensions::KHR_materials_transmission
		);

		const auto sceneIndex = GetDefaultSceneIndex(asset);
		const auto& scene = asset.scenes.at(sceneIndex);

		model.Name = scene.name;

		const auto directoryPath = filePath.parent_path();
		vector<StoredSkinJoints> storedSkinJoints;
		vector<LoadedTexture> loadedTextures;
		iterateSceneNodes(
			asset, sceneIndex, fastgltf::math::fmat4x4(),
			[&](fastgltf::Node& node, const fastgltf::math::fmat4x4& matrix) {
				if (node.meshIndex) {
					if (const auto& mesh = asset.meshes.at(node.meshIndex.value()); !empty(mesh.primitives)) {
						auto meshNode = make_shared<MeshNode>();

						meshNode->NodeName = node.name;
						meshNode->MeshName = mesh.name;

						meshNode->GlobalTransform = reinterpret_cast<const Matrix&>(matrix);

						if (node.skinIndex) {
							size_t skinJointCount = 0;
							if (const auto pStoredSkinJoints = ranges::find_if(storedSkinJoints, [&](const auto& value) {
								return value.Index == node.skinIndex.value();
								});
								pStoredSkinJoints == cend(storedSkinJoints)) {
								const auto& skin = asset.skins.at(node.skinIndex.value());
								if (skin.inverseBindMatrices) {
									auto skinJoints = make_shared<vector<SkinJoint>>();
									skinJoints->reserve(size(skin.joints));
									fastgltf::iterateAccessor<fastgltf::math::fmat4x4>(
										asset, asset.accessors.at(skin.inverseBindMatrices.value()),
										[&](const fastgltf::math::fmat4x4& value) {
											skinJoints->emplace_back(
												string(asset.nodes.at(skin.joints.at(size(*skinJoints))).name),
												reinterpret_cast<const Matrix&>(value)
											);
										}
									);

									skinJointCount = size(*skinJoints);

									if (!model.SkinJoints) {
										model.SkinJoints = make_shared<SkinJointDictionary>();
									}

									(*model.SkinJoints)[string(node.name)] = skinJoints;

									storedSkinJoints.emplace_back(node.skinIndex.value(), skinJoints);
								}
							}
							else {
								skinJointCount = size(*pStoredSkinJoints->SkinJoints);

								(*model.SkinJoints)[string(node.name)] = pStoredSkinJoints->SkinJoints;
							}

							if (skinJointCount) {
								meshNode->SkeletalTransforms = GPUBuffer::CreateDefault<XMFLOAT3X4>(commandList.GetDeviceContext(), skinJointCount);
							}
						}

						for (const auto& primitive : mesh.primitives) {
							if (const auto _primitive = ProcessPrimitive(
								directoryPath,
								asset, primitive,
								flipWindingOrder,
								model,
								loadedTextures,
								commandList
							)) {
								meshNode->Meshes.emplace_back(_primitive);
							}
						}

						model.MeshNodes.emplace_back(meshNode);
					}
				}
			}
		);
	}

	void LoadAnimation(AnimationCollection& animations, const path& filePath) {
		if (empty(filePath)) {
			throw invalid_argument("Animation file path cannot be empty");
		}

		auto asset = ::Load(
			filePath,
			fastgltf::Category::OnlyAnimations | fastgltf::Category::Nodes | fastgltf::Category::Scenes,
			fastgltf::Options::DecomposeNodeMatrices
		);

		const auto sceneIndex = GetDefaultSceneIndex(asset);
		const auto& scene = asset.scenes.at(sceneIndex);

		animations.Name = scene.name;

		vector<Animation::TargetNode> targetNodes;
		for (const auto nodeIndex : scene.nodeIndices) {
			const auto ReadNode = [&](this auto& self, const fastgltf::Node& node, Animation::TargetNode& targetNode) -> void {
				targetNode.Name = node.name;
				const auto& transform = get<fastgltf::TRS>(node.transform);
				targetNode.Transform = {
					.Translation = reinterpret_cast<const XMFLOAT3&>(transform.translation),
					.Rotation = reinterpret_cast<const XMFLOAT4&>(transform.rotation),
					.Scale = reinterpret_cast<const XMFLOAT3&>(transform.scale)
				};
				for (const auto child : node.children) {
					self(asset.nodes.at(child), targetNode.Children.emplace_back());
				}
			};
			ReadNode(asset.nodes.at(nodeIndex), targetNodes.emplace_back());
		}

		animations.reserve(size(asset.animations));
		for (const auto& animation : asset.animations) {
			float duration = 0;
			unordered_map<string, KeyframeCollection> keyframeCollections;
			for (const auto& channel : animation.channels) {
				if (!channel.nodeIndex) {
					continue;
				}

				const auto& sampler = animation.samplers.at(channel.samplerIndex);
				if (sampler.interpolation != fastgltf::AnimationInterpolation::Linear) {
					continue;
				}

				const auto
					& inputAccessor = asset.accessors.at(sampler.inputAccessor),
					& outputAccessor = asset.accessors.at(sampler.outputAccessor);
				auto& keyframeCollection = keyframeCollections[string(asset.nodes.at(channel.nodeIndex.value()).name)];
				switch (channel.path) {
					case fastgltf::AnimationPath::Translation:
					{
						if (empty(keyframeCollection.Translations)) {
							vector<KeyframeCollection::Translation> keys;
							keys.reserve(inputAccessor.count);
							fastgltf::iterateAccessor<float>(
								asset, inputAccessor,
								[&](float value) {
									keys.emplace_back(value);
							duration = max(duration, value);
								}
							);
							fastgltf::iterateAccessorWithIndex<XMFLOAT3>(
								asset, outputAccessor,
								[&](const XMFLOAT3& value, size_t index) {
									keys[index].Value = value;
								}
							);
							keyframeCollection.Translations.append_range(move(keys));
						}
					}
					break;

					case fastgltf::AnimationPath::Rotation:
					{
						if (empty(keyframeCollection.Rotations)) {
							vector<KeyframeCollection::Rotation> keys;
							keys.reserve(inputAccessor.count);
							fastgltf::iterateAccessor<float>(
								asset, inputAccessor,
								[&](float value) {
									keys.emplace_back(value);
							duration = max(duration, value);
								}
							);
							fastgltf::iterateAccessorWithIndex<XMFLOAT4>(
								asset, outputAccessor,
								[&](const XMFLOAT4& value, size_t index) {
									keys[index].Value = value;
								}
							);
							keyframeCollection.Rotations.append_range(move(keys));
						}
					}
					break;

					case fastgltf::AnimationPath::Scale:
					{
						if (empty(keyframeCollection.Scales)) {
							vector<KeyframeCollection::Scale> keys;
							keys.reserve(inputAccessor.count);
							fastgltf::iterateAccessor<float>(
								asset, inputAccessor,
								[&](float value) {
									keys.emplace_back(value);
							duration = max(duration, value);
								}
							);
							fastgltf::iterateAccessorWithIndex<XMFLOAT3>(
								asset, outputAccessor,
								[&](const XMFLOAT3& value, size_t index) {
									keys[index].Value = value;
								}
							);
							keyframeCollection.Scales.append_range(move(keys));
						}
					}
					break;
				}
			}
			animations.emplace_back(duration, move(keyframeCollections), targetNodes).Name = animation.name;
		}
	}
}
