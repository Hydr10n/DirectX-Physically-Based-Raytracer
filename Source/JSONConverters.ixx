module;

#include "JsonHelpers.h"

#include "directxtk12/SimpleMath.h"

export module JSONConverters;

import Math;

export {
	namespace DirectX::SimpleMath {
		JSON_CONVERSION1_FUNCTIONS(Vector2, ("X", x), ("Y", y));
		JSON_CONVERSION1_FUNCTIONS(Vector3, ("X", x), ("Y", y), ("Z", z));
		JSON_CONVERSION1_FUNCTIONS(Vector4, ("X", x), ("Y", y), ("Z", z), ("W", w));

		TO_JSON_FUNCTION_PROTOTYPE(Quaternion) { to_json(nlohmann_json_j, reinterpret_cast<const Vector4&>(nlohmann_json_t)); }
		FROM_JSON_FUNCTION_PROTOTYPE(Quaternion) {
			FROM_JSON1_FUNCTION_BODY(("Yaw", y), ("Pitch", x), ("Roll", z));
			if (nlohmann_json_t.x == 0 && nlohmann_json_t.y == 0 && nlohmann_json_t.z == 0) {
				from_json(nlohmann_json_j, reinterpret_cast<Vector4&>(nlohmann_json_t));
			}
			else {
				nlohmann_json_t = Quaternion::CreateFromYawPitchRoll(XMConvertToRadians(nlohmann_json_t.y), XMConvertToRadians(-nlohmann_json_t.x), XMConvertToRadians(-nlohmann_json_t.z));
			}
		}

		JSON_CONVERSION1_FUNCTIONS(Color, ("R", x), ("G", y), ("B", z), ("A", w));
	}

	namespace Math {
		JSON_CONVERSION_FUNCTIONS(AffineTransform, Translation, Rotation, Scale);
	}
}
