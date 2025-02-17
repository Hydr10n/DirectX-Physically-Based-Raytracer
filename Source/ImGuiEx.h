#pragma once

#include <functional>
#include <string>

#include "imgui_internal.h"

namespace ImGuiEx {
	struct Enablement {
		explicit Enablement(bool value) {
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, !value);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (value ? 1.0f : 0.5f));
		}

		~Enablement() {
			ImGui::PopStyleVar();
			ImGui::PopItemFlag();
		}
	};

	class Widget {
	public:
		explicit Widget(bool ret) : m_ret(ret) {}

		operator bool() const { return m_ret; }

	protected:
		bool m_ret;
	};

	struct Window : Widget {
		explicit Window(const char* name, bool* pIsOpen = nullptr, ImGuiWindowFlags flags = ImGuiWindowFlags_None) :
			Widget(ImGui::Begin(name, pIsOpen, flags)) {}

		~Window() { ImGui::End(); }
	};

	struct PopupModal : Widget {
		explicit PopupModal(const char* name, bool* pIsOpen = nullptr, ImGuiWindowFlags flags = ImGuiWindowFlags_None) :
			Widget(ImGui::BeginPopupModal(name, pIsOpen, flags)) {}

		~PopupModal() {
			if (m_ret) {
				ImGui::EndPopup();
			}
		}
	};

	struct MainMenuBar : Widget {
		MainMenuBar() : Widget(ImGui::BeginMainMenuBar()) {}

		~MainMenuBar() {
			if (m_ret) {
				ImGui::EndMainMenuBar();
			}
		}
	};

	struct Menu : Widget {
		explicit Menu(const char* label, bool isEnabled = true) : Widget(ImGui::BeginMenu(label, isEnabled)) {}

		~Menu() {
			if (m_ret) {
				ImGui::EndMenu();
			}
		}
	};

	struct TreeNode : Widget {
		explicit TreeNode(const char* label, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_None) :
			Widget(ImGui::TreeNodeEx(label, flags)) {}

		~TreeNode() {
			if (m_ret) {
				ImGui::TreePop();
			}
		}
	};

	struct Table : Widget {
		explicit Table(
			const char* ID, int columns, ImGuiTableFlags flags = ImGuiTableFlags_None,
			ImVec2 outerSize = {}, float innerWidth = 0
		) : Widget(ImGui::BeginTable(ID, columns, flags, outerSize, innerWidth)) {}

		~Table() {
			if (m_ret) {
				ImGui::EndTable();
			}
		}
	};

	inline void AlignForWidth(float width, float alignment = 0.5f) {
		if (const auto offset = (ImGui::GetContentRegionAvail().x - width) * alignment; offset > 0) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
		}
	}

	inline void AddUnderline(const ImColor& color) {
		auto itemRectMin = ImGui::GetItemRectMin(), itemRectMax = ImGui::GetItemRectMax();
		itemRectMin.y = itemRectMax.y;
		ImGui::GetWindowDrawList()->AddLine(itemRectMin, itemRectMax, color);
	}

	inline auto Hyperlink(const char* label, const char* link) {
		const auto& style = ImGui::GetStyle();
		ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_ButtonHovered]);
		ImGui::Text(label);
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) {
			AddUnderline(style.Colors[ImGuiCol_ButtonHovered]);
			ImGui::SetTooltip(link);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				return true;
			}
		}
		else {
			AddUnderline(style.Colors[ImGuiCol_Button]);
		}
		return false;
	}

	inline ImGuiInputEvent* FindLatestInputEvent(ImGuiContext* context, ImGuiInputEventType type, int arg = -1) {
		for (auto i = context->InputEventsQueue.Size - 1; i >= 0; i--) {
			if (auto& e = context->InputEventsQueue[i];
				e.Type == type
				&& (type != ImGuiInputEventType_Key || e.Key.Key == arg)
				&& (type != ImGuiInputEventType_MouseButton || e.MouseButton.Button == arg)) {
				return &e;
			}
		}
		return nullptr;
	}

	template <typename T, template <typename> typename Container = std::initializer_list>
	auto Combo(
		const char* label,
		const Container<T>& values, T previewValue, T& value,
		std::function<std::string(T)> ToString,
		std::function<ImGuiSelectableFlags(T)> ToSelectableFlags = nullptr,
		ImGuiComboFlags flags = ImGuiComboFlags_None
	) {
		auto ret = false;
		if (ImGui::BeginCombo(label, ToString(previewValue).c_str(), flags)) {
			for (const auto v : values) {
				const auto isSelected = value == v;
				if (ImGui::Selectable(ToString(v).c_str(), isSelected, ToSelectableFlags == nullptr ? ImGuiSelectableFlags_None : ToSelectableFlags(v))) {
					value = v;
					ret = true;
				}
				if (isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		return ret;
	}

	inline void Spinner(const char* label, ImU32 color, float radius, float thickness = 1) {
		auto& window = *ImGui::GetCurrentWindow();
		if (window.SkipItems) {
			return;
		}

		const auto ID = window.GetID(label);
		const auto& context = *ImGui::GetCurrentContext();
		const auto& style = context.Style;

		const auto pos = window.DC.CursorPos;

		const ImRect rect(pos, ImVec2(pos.x + 2 * radius, pos.y + 2 * (radius + style.FramePadding.y)));
		ImGui::ItemSize(rect, style.FramePadding.y);
		if (!ImGui::ItemAdd(rect, ID)) {
			return;
		}

		window.DrawList->PathClear();

		constexpr auto SegmentCount = 30;
		const auto
			time = static_cast<float>(context.Time),
			start = ImAbs(ImSin(time * 1.8f) * (SegmentCount - 5)),
			min = 2 * IM_PI * start / static_cast<float>(SegmentCount - 3),
			max = 2 * IM_PI * static_cast<float>(SegmentCount) / static_cast<float>(SegmentCount);

		const ImVec2 center(pos.x + radius, pos.y + radius + style.FramePadding.y);
		for (int i = 0; i < SegmentCount; i++) {
			const auto value = min + static_cast<float>(i) / static_cast<float>(SegmentCount) * (max - min);
			window.DrawList->PathLineTo(ImVec2(center.x + ImCos(value + time * 8) * radius, center.y + ImSin(value + time * 8) * radius));
		}

		window.DrawList->PathStroke(color, ImDrawFlags_None, thickness);
	}
}
