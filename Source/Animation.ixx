module;

#include <filesystem>
#include <ranges>

#include "assimp/Importer.hpp"
#include "assimp/postprocess.h"
#include "assimp/scene.h"

#include "directxtk12/SimpleMath.h"

export module Animation;

import Model;
import ResourceHelpers;

using namespace Assimp;
using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace ResourceHelpers;
using namespace std;
using namespace std::filesystem;

namespace {
	struct KeyframeCollection {
		template <typename T>
		struct Key {
			double Time;
			T Value;
		};

		struct PositionKey : Key<Vector3> {};
		vector<PositionKey> PositionKeys;

		struct RotationKey : Key<Quaternion> {};
		vector<RotationKey> RotationKeys;

		struct ScalingKey : Key<Vector3> {};
		vector<ScalingKey> ScalingKeys;

		KeyframeCollection() = default;

		KeyframeCollection(const aiNodeAnim& channel) {
			for (const auto i : views::iota(0u, channel.mNumPositionKeys)) {
				PositionKeys.emplace_back(PositionKey{ channel.mPositionKeys[i].mTime, reinterpret_cast<const XMFLOAT3&>(channel.mPositionKeys[i].mValue) });
			}
			for (const auto i : views::iota(0u, channel.mNumRotationKeys)) {
				constexpr auto ToQuaternion = [](const aiQuaternion& value) { return Quaternion(value.x, value.y, value.z, value.w); };
				RotationKeys.emplace_back(RotationKey{ channel.mRotationKeys[i].mTime, ToQuaternion(channel.mRotationKeys[i].mValue) });
			}
			for (const auto i : views::iota(0u, channel.mNumScalingKeys)) {
				ScalingKeys.emplace_back(ScalingKey{ channel.mScalingKeys[i].mTime, reinterpret_cast<const XMFLOAT3&>(channel.mScalingKeys[i].mValue) });
			}
		}

		template <typename T>
		static size_t FindKey(const vector<T>& keys, double time) {
			const auto pKey = ranges::upper_bound(keys, T{ time }, [](const auto& a, const auto& b) { return a.Time < b.Time; });
			return pKey == cend(keys) ? size(keys) : min(static_cast<size_t>(pKey - cbegin(keys) - 1), size(keys));
		}

		template <typename T>
		static Matrix Interpolate(const vector<T>& keys, double time) {
			decltype(T::Value) value;
			const auto i = FindKey(keys, time);
			if (i == size(keys)) value = keys.back().Value;
			else {
				const auto& key1 = keys[i], & key2 = keys[i + 1];
				const auto t = static_cast<float>((time - key1.Time) / (key2.Time - key1.Time));
				if constexpr (is_same_v<T, PositionKey> || is_same_v<T, ScalingKey>) value = Vector3::Lerp(key1.Value, key2.Value, t);
				else if constexpr (is_same_v<T, RotationKey>) value = Quaternion::Slerp(key1.Value, key2.Value, t);
			}
			if constexpr (is_same_v<T, PositionKey>) return Matrix::CreateTranslation(value);
			if constexpr (is_same_v<T, RotationKey>) return Matrix::CreateFromQuaternion(value);
			if constexpr (is_same_v<T, ScalingKey>) return Matrix::CreateScale(value);
		}

		auto Interpolate(double time) const { return Interpolate(ScalingKeys, time) * Interpolate(RotationKeys, time) * Interpolate(PositionKeys, time); }
	};
}

export {
	struct Animation {
		const auto& GetName() const { return m_name; }

		auto GetTicksPerSecond() const { return m_ticksPerSecond; }

		auto GetDuration() const { return m_duration / m_ticksPerSecond; }

		auto GetTime() const { return m_time / m_ticksPerSecond; }
		void SetTime(double seconds = 0) { m_time = clamp(seconds * m_ticksPerSecond, 0.0, m_duration); }

		void Tick(double elapsedSeconds) {
			m_time = fmod(m_time + elapsedSeconds * m_ticksPerSecond, m_duration);

			ComputeTransforms();
		}

		const auto& GetGlobalTransforms() const { return m_globalTransforms; }

		const auto& GetSkeletalTransforms() const { return m_skeletalTransforms; }

		void ComputeTransforms() {
			const auto ComputeTransforms = [&](this auto& self, const TransformNode& transformNode, const Matrix& parentTransform) -> void {
				const auto pKeyframeCollection = m_keyframeCollections.find(transformNode.Name);
				const auto transform = (pKeyframeCollection == cend(m_keyframeCollections) ? transformNode.Transform : pKeyframeCollection->second.Interpolate(static_cast<float>(m_time))) * parentTransform;

				m_globalTransforms[transformNode.Name] = transform;
				if (transformNode.BoneInfo.ID != ~0u) XMStoreFloat3x4(&m_skeletalTransforms[transformNode.BoneInfo.ID], transformNode.BoneInfo.Transform * transform * m_skeletalRootInverseGlobalTransform);

				for (const auto& child : transformNode.Children) self(child, transform);
			};
			ComputeTransforms(m_rootTransformNode, {});
		}

	private:
		string m_name;

		double m_ticksPerSecond{}, m_duration{}, m_time{};

		unordered_map<string, KeyframeCollection> m_keyframeCollections;

		struct TransformNode {
			string Name;
			Matrix Transform;
			BoneInfo BoneInfo;
			vector<TransformNode> Children;
		};
		TransformNode m_rootTransformNode;

		unordered_map<string, Matrix> m_globalTransforms;

		Matrix m_skeletalRootInverseGlobalTransform;
		vector<XMFLOAT3X4> m_skeletalTransforms;

		void Load(const aiScene& scene, const aiAnimation& animation) {
			m_name = animation.mName.C_Str();

			m_ticksPerSecond = animation.mTicksPerSecond;
			m_duration = animation.mDuration;

			for (const auto i : views::iota(0u, animation.mNumChannels)) m_keyframeCollections[animation.mChannels[i]->mNodeName.C_Str()] = *animation.mChannels[i];

			const auto ReadNode = [](this auto& self, const aiNode& node, TransformNode& transformNode) -> void {
				transformNode.Name = node.mName.C_Str();
				transformNode.Transform = reinterpret_cast<const Matrix&>(node.mTransformation).Transpose();
				for (const auto i : views::iota(0u, node.mNumChildren)) self(*node.mChildren[i], transformNode.Children.emplace_back());
			};
			ReadNode(*scene.mRootNode, m_rootTransformNode);
		}

		void Bind(const BoneInfoDictionary& boneInfo) {
			bool isSkeletalRootFound = false;
			const auto StoreBoneInfo = [&](this auto& self, TransformNode& transformNode, const Matrix& parentTransform) -> void {
				const auto transform = transformNode.Transform * parentTransform;
				if (const auto pBoneInfo = boneInfo.find(transformNode.Name); pBoneInfo != cend(boneInfo)) {
					transformNode.BoneInfo = pBoneInfo->second;

					if (!isSkeletalRootFound) {
						m_skeletalRootInverseGlobalTransform = transform.Invert();
						isSkeletalRootFound = true;
					}
				}
				for (auto& child : transformNode.Children) self(child, transform);
			};
			StoreBoneInfo(m_rootTransformNode, {});

			m_skeletalTransforms.resize(size(boneInfo));
		}

		friend struct AnimationCollection;
	};

	struct AnimationCollection : vector<Animation> {
		using vector::vector;

		string Name;

		void Load(const path& filePath) {
			if (::empty(filePath)) throw invalid_argument("Animation file path cannot be empty");

			Importer importer;
			importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, ~aiComponent_ANIMATIONS);
			const auto scene = importer.ReadFile(reinterpret_cast<const char*>(filePath.u8string().c_str()), aiProcess_RemoveComponent | aiProcess_ConvertToLeftHanded);
			if (scene == nullptr || scene->mRootNode == nullptr) throw runtime_error(format("Assimp: {}", importer.GetErrorString()));

			Name = scene->mName.C_Str();

			for (const auto i : views::iota(0u, scene->mNumAnimations)) emplace_back(Animation()).Load(*scene, *scene->mAnimations[i]);
		}

		auto HasBoneInfo() const { return m_hasBoneInfo; }
		void SetBoneInfo(const BoneInfoDictionary& boneInfo) {
			m_hasBoneInfo = !::empty(boneInfo);

			for (auto& animation : *this) animation.Bind(boneInfo);
		}

		auto GetSelectedIndex() const { return min(m_selectedIndex, size() - 1); }
		void SetSelectedIndex(size_t index) { m_selectedIndex = min(index, size() - 1); }

	private:
		bool m_hasBoneInfo{};

		size_t m_selectedIndex{};
	};
}

struct AnimationCollectionDictionaryLoader {
	void operator()(AnimationCollection& resource, const path& filePath) const { resource.Load(filePath); }
};

export using AnimationCollectionDictionary = ResourceDictionary<string, AnimationCollection, AnimationCollectionDictionaryLoader>;
