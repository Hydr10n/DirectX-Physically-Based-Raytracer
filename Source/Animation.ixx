module;

#include <algorithm>
#include <memory>
#include <ranges>

#include "directxtk12/SimpleMath.h"

export module Animation;

import Math;
import Model;

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace Math;
using namespace std;

export {
	struct KeyframeCollection {
		struct Translation {
			double Time{};
			Vector3 Value;
		};
		vector<Translation> Translations;

		struct Rotation {
			double Time{};
			Quaternion Value;
		};
		vector<Rotation> Rotations;

		struct Scale {
			double Time{};
			Vector3 Value;
		};
		vector<Scale> Scales;

		template <typename T>
		static size_t FindKey(const vector<T>& keys, double time) {
			const auto pKey = ranges::upper_bound(keys, T{ time }, [](const auto& a, const auto& b) { return a.Time < b.Time; });
			return pKey == cend(keys) ? size(keys) : static_cast<size_t>(distance(cbegin(keys), pKey) - 1);
		}

		template <typename T>
		static Matrix Interpolate(const vector<T>& keys, double time) {
			decltype(T::Value) value;
			if (time < keys.front().Time) {
				value = keys.front().Value;
			}
			else if (const auto i = FindKey(keys, time);
				i == size(keys)) {
				value = keys.back().Value;
			}
			else {
				const auto& key0 = keys[i], & key1 = keys[i + 1];
				const auto t = static_cast<float>((time - key0.Time) / (key1.Time - key0.Time));
				if constexpr (is_same_v<T, Translation> || is_same_v<T, Scale>) {
					value = Vector3::Lerp(key0.Value, key1.Value, t);
				}
				else if constexpr (is_same_v<T, Rotation>) {
					value = Quaternion::Slerp(key0.Value, key1.Value, t);
				}
			}
			if constexpr (is_same_v<T, Translation>) {
				return Matrix::CreateTranslation(value);
			}
			if constexpr (is_same_v<T, Rotation>) {
				return Matrix::CreateFromQuaternion(value);
			}
			if constexpr (is_same_v<T, Scale>) {
				return Matrix::CreateScale(value);
			}
			throw;
		}
	};

	struct Animation {
		string Name;

		struct TargetNode {
			string Name;
			AffineTransform Transform;
			vector<TargetNode> Children;
		};

		Animation(
			double duration,
			unordered_map<string, KeyframeCollection>&& keyframeCollections,
			const vector<TargetNode>& targetNode
		) : m_duration(duration), m_keyframeCollections(move(keyframeCollections)), m_targetNodes(targetNode) {}

		void Bind(const shared_ptr<SkinJointDictionary>& skinJoints) {
			m_skinJoints = skinJoints;

			m_skeletalTransforms = {};
			if (skinJoints) {
				for (const auto& [Name, SkinJoints] : *skinJoints) {
					m_skeletalTransforms[Name].resize(size(*SkinJoints));
				}
			}
		}

		auto GetDuration() const { return m_duration; }

		auto GetTime() const { return m_time; }
		void SetTime(double seconds = 0) { m_time = clamp(seconds, 0.0, m_duration); }

		void Tick(double elapsedSeconds) {
			m_time = fmod(m_time + elapsedSeconds, m_duration);

			ComputeTransforms();
		}

		const auto& GetGlobalTransforms() const { return m_globalTransforms; }

		const auto& GetSkeletalTransforms() const { return m_skeletalTransforms; }

		void ComputeTransforms() {
			for (const auto& targetNode : m_targetNodes) {
				const auto ComputeTransforms = [&](this auto& self, const TargetNode& node, const Matrix& parentTransform) -> void {
					Matrix transform;
					if (const auto pKeyframeCollection = m_keyframeCollections.find(node.Name);
						pKeyframeCollection == cend(m_keyframeCollections)) {
						transform = node.Transform();
					}
					else {
						const auto& keyframeCollection = pKeyframeCollection->second;
						transform =
							(empty(keyframeCollection.Scales) ? Matrix::CreateScale(node.Transform.Scale) :
								keyframeCollection.Interpolate(keyframeCollection.Scales, m_time))
							* (empty(keyframeCollection.Rotations) ? Matrix::CreateFromQuaternion(node.Transform.Rotation) :
								keyframeCollection.Interpolate(keyframeCollection.Rotations, m_time))
							* (empty(keyframeCollection.Translations) ? Matrix::CreateTranslation(node.Transform.Translation) :
								keyframeCollection.Interpolate(keyframeCollection.Translations, m_time));
					}
					transform *= parentTransform;
					m_globalTransforms[node.Name] = transform;
					for (const auto& child : node.Children) {
						self(child, transform);
					}
				};
				ComputeTransforms(targetNode, {});
			}

			if (m_skinJoints) {
				for (const auto& [Name, SkinJoints] : *m_skinJoints) {
					if (const auto pGlobalTransform = m_globalTransforms.find(Name);
						pGlobalTransform != cend(m_globalTransforms)) {
						auto& skeletalTransforms = m_skeletalTransforms.at(Name);
						const auto inverseGlobalTransform = pGlobalTransform->second.Invert();
						for (size_t i = 0; const auto & skinJoint : *SkinJoints) {
							if (const auto pGlobalTransform = m_globalTransforms.find(skinJoint.Name);
								pGlobalTransform != cend(m_globalTransforms)) {
								XMStoreFloat3x4(&skeletalTransforms[i], skinJoint.InverseBindMatrix * pGlobalTransform->second * inverseGlobalTransform);
							}
							i++;
						}
					}
				}
			}
		}

	private:
		double m_duration, m_time{};

		unordered_map<string, KeyframeCollection> m_keyframeCollections;

		vector<TargetNode> m_targetNodes;

		unordered_map<string, Matrix> m_globalTransforms;

		shared_ptr<SkinJointDictionary> m_skinJoints;
		unordered_map<string, vector<XMFLOAT3X4>> m_skeletalTransforms;
	};

	struct AnimationCollection : vector<Animation> {
		using vector::vector;

		string Name;

		auto GetSelectedIndex() const { return min(m_selectedIndex, size() - 1); }
		void SetSelectedIndex(size_t index) { m_selectedIndex = min(index, size() - 1); }

		auto IsSkinned() const { return m_isSkinned; }
		void Bind(const shared_ptr<SkinJointDictionary>& skinJoints) {
			m_isSkinned = skinJoints != nullptr;
			for (auto& animation : *this) {
				animation.Bind(skinJoints);
			}
		}

	private:
		size_t m_selectedIndex{};

		bool m_isSkinned{};
	};
}
