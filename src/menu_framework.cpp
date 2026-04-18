#include "menu_framework.h"
#include "hook_touch_event.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace {
MenuSettingSpec* g_active_shortcut_editor = nullptr;

struct JsonValue {
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Object,
    };

    Type type = Type::Null;
    bool bool_value = false;
    double number_value = 0.0;
    std::string string_value;
    std::map<std::string, JsonValue> object_value;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    bool Parse(JsonValue& out, std::string& error) {
        SkipWhitespace();
        if (!ParseValue(out, error)) return false;
        SkipWhitespace();
        if (pos_ != input_.size()) {
            error = "unexpected trailing characters";
            return false;
        }
        return true;
    }

private:
    void SkipWhitespace() {
        while (pos_ < input_.size()) {
            char c = input_[pos_];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                ++pos_;
                continue;
            }
            break;
        }
    }

    bool ParseValue(JsonValue& out, std::string& error) {
        SkipWhitespace();
        if (pos_ >= input_.size()) {
            error = "unexpected end of input";
            return false;
        }

        char c = input_[pos_];
        if (c == '{') return ParseObject(out, error);
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return ParseString(out.string_value, error);
        }
        if (c == 't') return ParseLiteral("true", JsonValue::Type::Bool, out, error);
        if (c == 'f') return ParseLiteral("false", JsonValue::Type::Bool, out, error);
        if (c == 'n') return ParseLiteral("null", JsonValue::Type::Null, out, error);
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out, error);

        error = "unexpected token";
        return false;
    }

    bool ParseObject(JsonValue& out, std::string& error) {
        out.type = JsonValue::Type::Object;
        out.object_value.clear();
        ++pos_;
        SkipWhitespace();
        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            return true;
        }

        while (pos_ < input_.size()) {
            std::string key;
            if (!ParseString(key, error)) return false;
            SkipWhitespace();
            if (pos_ >= input_.size() || input_[pos_] != ':') {
                error = "expected ':'";
                return false;
            }
            ++pos_;

            JsonValue value;
            if (!ParseValue(value, error)) return false;
            out.object_value.emplace(std::move(key), std::move(value));

            SkipWhitespace();
            if (pos_ >= input_.size()) {
                error = "unexpected end of object";
                return false;
            }
            if (input_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (input_[pos_] != ',') {
                error = "expected ','";
                return false;
            }
            ++pos_;
            SkipWhitespace();
        }

        error = "unterminated object";
        return false;
    }

    bool ParseString(std::string& out, std::string& error) {
        SkipWhitespace();
        if (pos_ >= input_.size() || input_[pos_] != '"') {
            error = "expected string";
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < input_.size()) {
            char c = input_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= input_.size()) {
                    error = "unterminated escape";
                    return false;
                }
                char esc = input_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default:
                        error = "unsupported escape";
                        return false;
                }
                continue;
            }
            out.push_back(c);
        }
        error = "unterminated string";
        return false;
    }

    bool ParseLiteral(const char* literal, JsonValue::Type type, JsonValue& out, std::string& error) {
        const size_t len = std::strlen(literal);
        if (input_.compare(pos_, len, literal) != 0) {
            error = "invalid literal";
            return false;
        }
        pos_ += len;
        out.type = type;
        if (type == JsonValue::Type::Bool) {
            out.bool_value = (literal[0] == 't');
        }
        return true;
    }

    bool ParseNumber(JsonValue& out, std::string& error) {
        const size_t start = pos_;
        if (input_[pos_] == '-') ++pos_;
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }

        const std::string token = input_.substr(start, pos_ - start);
        char* end_ptr = nullptr;
        const double parsed = std::strtod(token.c_str(), &end_ptr);
        if (end_ptr == token.c_str() || *end_ptr != '\0') {
            error = "invalid number";
            return false;
        }
        out.type = JsonValue::Type::Number;
        out.number_value = parsed;
        return true;
    }

    const std::string& input_;
    size_t pos_ = 0;
};

static std::string EscapeJsonString(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

static void DrawSectionTitle(const char* title) {
    ImGui::Spacing();
    ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 text_size = ImGui::CalcTextSize(title);
    float content_width = ImGui::GetContentRegionAvail().x;
    float line_y = start.y + text_size.y * 0.55f;
    float left_end = start.x + 12.0f;
    float text_x = left_end + 10.0f;
    float right_start = text_x + text_size.x + 10.0f;
    float right_end = start.x + content_width - 12.0f;
    ImU32 line_color = ImGui::GetColorU32(ImVec4(0.24f, 0.62f, 1.00f, 0.72f));
    ImU32 text_color = ImGui::GetColorU32(ImVec4(0.84f, 0.93f, 1.00f, 0.98f));

    draw_list->AddLine(ImVec2(start.x, line_y), ImVec2(left_end, line_y), line_color, 1.8f);
    if (right_end > right_start) {
        draw_list->AddLine(ImVec2(right_start, line_y), ImVec2(right_end, line_y), line_color, 1.8f);
    }
    draw_list->AddText(ImVec2(text_x, start.y), text_color, title);
    ImGui::Dummy(ImVec2(content_width, text_size.y + 12.0f));
}

static void BeginSectionFrame(const char* title, ImVec2& out_start) {
    ImGui::PushID(title);
    ImGui::BeginGroup();
    out_start = ImGui::GetCursorScreenPos();
    DrawSectionTitle(title);
}

static void EndSectionFrame(const ImVec2& start) {
    ImGui::EndGroup();
    ImVec2 end = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRect(
        ImVec2(start.x, start.y + 4.0f),
        ImVec2(end.x, end.y + 8.0f),
        ImGui::GetColorU32(ImVec4(0.18f, 0.40f, 0.72f, 0.72f)),
        10.0f,
        0,
        1.5f);
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::PopID();
}

static void DrawTooltip(const std::string& tooltip) {
    if (tooltip.empty()) return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        const float tooltip_width = 360.0f;
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(220.0f, 0.0f),
            ImVec2(tooltip_width, FLT_MAX));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tooltip_width);
        ImGui::TextWrapped("%s", tooltip.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static bool DrawProgressSliderFloat(const char* label, float* value, float min_value, float max_value, const char* format) {
    ImGui::TextUnformatted(label);
    const float slider_rounding = 14.0f;
    const ImVec2 slider_padding(8.0f, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.18f, 0.30f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.14f, 0.26f, 0.42f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.16f, 0.30f, 0.48f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.20f, 0.66f, 1.00f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.20f, 0.66f, 1.00f, 0.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, slider_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, slider_padding);
    bool changed = ImGui::SliderFloat((std::string("##") + label).c_str(), value, min_value, max_value, "");
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);

    ImVec2 slider_min = ImGui::GetItemRectMin();
    ImVec2 slider_max = ImGui::GetItemRectMax();
    float display_t = 0.0f;
    if (max_value > min_value) {
        display_t = std::clamp((*value - min_value) / (max_value - min_value), 0.0f, 1.0f);
    }
    float fill_x = slider_min.x + (slider_max.x - slider_min.x) * display_t;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilledMultiColor(
        slider_min,
        ImVec2(fill_x, slider_max.y),
        ImGui::GetColorU32(ImVec4(0.10f, 0.46f, 0.94f, 0.94f)),
        ImGui::GetColorU32(ImVec4(0.22f, 0.70f, 1.00f, 0.98f)),
        ImGui::GetColorU32(ImVec4(0.22f, 0.70f, 1.00f, 0.98f)),
        ImGui::GetColorU32(ImVec4(0.10f, 0.46f, 0.94f, 0.94f)));
    char value_text[64];
    const float abs_value = std::fabs(*value);
    if (abs_value > 0.0f && abs_value < 1.0f) {
        std::snprintf(value_text, sizeof(value_text), "%.4f", *value);
    } else {
        std::snprintf(value_text, sizeof(value_text), format, *value);
    }
    ImVec2 text_size = ImGui::CalcTextSize(value_text);
    ImVec2 text_pos(
        slider_min.x + ((slider_max.x - slider_min.x) - text_size.x) * 0.5f,
        slider_min.y + ((slider_max.y - slider_min.y) - text_size.y) * 0.5f);
    draw_list->AddText(
        ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f),
        ImGui::GetColorU32(ImVec4(0.02f, 0.08f, 0.16f, 0.85f)),
        value_text);
    draw_list->AddText(
        text_pos,
        ImGui::GetColorU32(ImVec4(0.94f, 0.98f, 1.00f, 1.00f)),
        value_text);
    return changed;
}

static bool DrawProgressSliderInt(const char* label, int* value, int min_value, int max_value, const char* format) {
    ImGui::TextUnformatted(label);
    char value_text[64];
    std::snprintf(value_text, sizeof(value_text), format, *value);

    const float slider_rounding = 14.0f;
    const ImVec2 slider_padding(8.0f, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.18f, 0.30f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.14f, 0.26f, 0.42f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.16f, 0.30f, 0.48f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.20f, 0.66f, 1.00f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.20f, 0.66f, 1.00f, 0.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, slider_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, slider_padding);
    bool changed = ImGui::SliderInt((std::string("##") + label).c_str(), value, min_value, max_value, "");
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);

    ImVec2 slider_min = ImGui::GetItemRectMin();
    ImVec2 slider_max = ImGui::GetItemRectMax();
    float t = static_cast<float>(*value - min_value) / static_cast<float>(max_value - min_value);
    t = std::clamp(t, 0.0f, 1.0f);
    float fill_x = slider_min.x + (slider_max.x - slider_min.x) * t;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilledMultiColor(
        slider_min,
        ImVec2(fill_x, slider_max.y),
        ImGui::GetColorU32(ImVec4(0.10f, 0.46f, 0.94f, 0.94f)),
        ImGui::GetColorU32(ImVec4(0.22f, 0.70f, 1.00f, 0.98f)),
        ImGui::GetColorU32(ImVec4(0.22f, 0.70f, 1.00f, 0.98f)),
        ImGui::GetColorU32(ImVec4(0.10f, 0.46f, 0.94f, 0.94f)));
    ImVec2 text_size = ImGui::CalcTextSize(value_text);
    ImVec2 text_pos(
        slider_min.x + ((slider_max.x - slider_min.x) - text_size.x) * 0.5f,
        slider_min.y + ((slider_max.y - slider_min.y) - text_size.y) * 0.5f);
    draw_list->AddText(
        ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f),
        ImGui::GetColorU32(ImVec4(0.02f, 0.08f, 0.16f, 0.85f)),
        value_text);
    draw_list->AddText(
        text_pos,
        ImGui::GetColorU32(ImVec4(0.94f, 0.98f, 1.00f, 1.00f)),
        value_text);
    return changed;
}

static bool GetObjectMember(const JsonValue& parent, const std::string& key, const JsonValue*& out) {
    if (parent.type != JsonValue::Type::Object) return false;
    auto it = parent.object_value.find(key);
    if (it == parent.object_value.end()) return false;
    out = &it->second;
    return true;
}

static ImVec2 ComputeDefaultShortcutPos(size_t order_index, const ImVec2& display_size) {
    const float start_x = 24.0f;
    const float start_y = 150.0f;
    const float spacing_y = 56.0f;
    const float widget_width = 188.0f;
    const float widget_height = 44.0f;

    ImVec2 pos(start_x, start_y + spacing_y * static_cast<float>(order_index));
    pos.x = std::clamp(pos.x, 8.0f, std::max(8.0f, display_size.x - widget_width - 8.0f));
    pos.y = std::clamp(pos.y, 8.0f, std::max(8.0f, display_size.y - widget_height - 8.0f));
    return pos;
}

static ImVec2 ClampShortcutSize(const ImVec2& size) {
    return ImVec2(std::clamp(size.x, 120.0f, 360.0f), std::clamp(size.y, 38.0f, 180.0f));
}

static bool IsShortcutEditorActive() {
    return g_active_shortcut_editor != nullptr;
}

}  // namespace

bool MenuSettingSpec::DrawShortcutEditor() {
    if (!shortcut_supported_) return false;

    bool changed = false;
    ImGui::PushID((key_ + "_shortcut").c_str());
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::Indent(26.0f);

    const bool is_editing = g_active_shortcut_editor == this;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, is_editing ? ImVec4(0.22f, 0.54f, 0.88f, 0.88f)
                                                      : ImVec4(0.12f, 0.18f, 0.28f, 0.58f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.42f, 0.64f, 0.78f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.28f, 0.52f, 0.78f, 0.88f));
    if (ImGui::SmallButton(is_editing ? u8"⚙ 编辑中" : u8"⚙ Shortcut")) {
        shortcut_editor_open_ = !is_editing;
        g_active_shortcut_editor = shortcut_editor_open_ ? this : nullptr;
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    if (shortcut_editor_open_ && g_active_shortcut_editor == this) {
        ImGui::SameLine();
        changed |= ImGui::Checkbox("启用悬浮", &shortcut_enabled_);

        int mode_index = static_cast<int>(shortcut_mode_);
        const char* mode_labels[] = {"Hold", "Toggle"};
        if (ImGui::Combo("模式", &mode_index, mode_labels, IM_ARRAYSIZE(mode_labels))) {
            shortcut_mode_ = static_cast<MenuShortcutMode>(mode_index);
            changed = true;
        }

        ImVec2 size = ClampShortcutSize(shortcut_size_);
        if (ImGui::SliderFloat("宽度", &size.x, 120.0f, 360.0f, "%.0f")) {
            shortcut_size_ = ClampShortcutSize(size);
            changed = true;
        }
        size = ClampShortcutSize(shortcut_size_);
        if (ImGui::SliderFloat("高度", &size.y, 38.0f, 180.0f, "%.0f")) {
            shortcut_size_ = ClampShortcutSize(size);
            changed = true;
        }

        if (ImGui::Button("重置位置")) {
            shortcut_pos_ = ImVec2(-1.0f, -1.0f);
            shortcut_hold_active_ = false;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("重置大小")) {
            shortcut_size_ = default_shortcut_size_;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("完成编辑")) {
            shortcut_editor_open_ = false;
            if (g_active_shortcut_editor == this) {
                g_active_shortcut_editor = nullptr;
            }
        }
    }
    ImGui::Unindent(26.0f);
    ImGui::PopID();
    return changed;
}

bool MenuSettingSpec::RenderShortcutWidget(size_t order_index) {
    if (!shortcut_supported_ || !shortcut_enabled_ || !bool_value_) return false;

    ImGuiIO& io = ImGui::GetIO();
    if (shortcut_pos_.x < 0.0f || shortcut_pos_.y < 0.0f) {
        shortcut_pos_ = default_shortcut_pos_.x >= 0.0f && default_shortcut_pos_.y >= 0.0f
            ? default_shortcut_pos_
            : ComputeDefaultShortcutPos(order_index, io.DisplaySize);
    }

    shortcut_size_ = ClampShortcutSize(shortcut_size_);
    const ImVec2 widget_size = shortcut_size_;
    shortcut_pos_.x = std::clamp(shortcut_pos_.x, 8.0f, std::max(8.0f, io.DisplaySize.x - widget_size.x - 8.0f));
    shortcut_pos_.y = std::clamp(shortcut_pos_.y, 8.0f, std::max(8.0f, io.DisplaySize.y - widget_size.y - 8.0f));

    ImGui::SetNextWindowPos(shortcut_pos_, ImGuiCond_Always);
    ImGui::SetNextWindowSize(widget_size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.18f);

    const std::string window_name = "##shortcut_widget_" + key_;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoMove;

    bool value_changed = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.11f, 0.18f, 0.42f));
    if (ImGui::Begin(window_name.c_str(), nullptr, flags)) {
        const bool is_editing = g_active_shortcut_editor == this;
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 widget_min = ImGui::GetCursorScreenPos();
        const ImVec2 widget_max(widget_min.x + widget_size.x, widget_min.y + widget_size.y);
        ImGui::InvisibleButton("##shortcut_surface", widget_size);
        TouchPoint touches[10];
        const int touch_count = (!is_editing && has_active_touch_points())
            ? get_active_touch_points(touches, 10)
            : 0;
        bool touch_inside = false;
        for (int i = 0; i < touch_count; ++i) {
            if (!touches[i].active) continue;
            if (touches[i].x >= widget_min.x && touches[i].x <= widget_max.x &&
                touches[i].y >= widget_min.y && touches[i].y <= widget_max.y) {
                touch_inside = true;
                break;
            }
        }
        const bool hovered = touch_inside || ImGui::IsItemHovered();
        const bool mouse_held = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool mouse_clicked = ImGui::IsItemClicked();
        const bool use_multi_touch_logic = !is_editing && touch_count > 0;
        const bool held = use_multi_touch_logic ? touch_inside : mouse_held;
        const bool clicked = use_multi_touch_logic
            ? (touch_inside && !shortcut_touch_active_)
            : mouse_clicked;

        if (is_editing) {
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                shortcut_pos_.x += io.MouseDelta.x;
                shortcut_pos_.y += io.MouseDelta.y;
            }
        } else if (shortcut_mode_ == MenuShortcutMode::Toggle) {
            if (clicked) {
                *bool_value_ = !*bool_value_;
                value_changed = true;
            }
            shortcut_touch_active_ = touch_inside;
        } else {
            if (held && !shortcut_hold_active_) {
                shortcut_hold_active_ = true;
                shortcut_hold_restore_value_ = *bool_value_;
                if (!*bool_value_) {
                    *bool_value_ = true;
                    value_changed = true;
                }
            }
            if (!held && shortcut_hold_active_) {
                shortcut_hold_active_ = false;
                if (*bool_value_ != shortcut_hold_restore_value_) {
                    *bool_value_ = shortcut_hold_restore_value_;
                    value_changed = true;
                }
            }
            shortcut_touch_active_ = touch_inside;
        }

        const ImU32 fill_color = ImGui::GetColorU32(
            is_editing ? ImVec4(0.16f, 0.34f, 0.58f, 0.88f)
            : (*bool_value_ ? (held ? ImVec4(0.20f, 0.68f, 1.00f, 0.96f)
                                    : hovered ? ImVec4(0.22f, 0.64f, 0.98f, 0.88f)
                                              : ImVec4(0.18f, 0.58f, 0.94f, 0.80f))
                            : (held ? ImVec4(0.20f, 0.26f, 0.34f, 0.90f)
                                    : hovered ? ImVec4(0.18f, 0.22f, 0.30f, 0.82f)
                                              : ImVec4(0.12f, 0.16f, 0.24f, 0.74f))));
        const ImU32 border_color = ImGui::GetColorU32(
            is_editing ? ImVec4(0.28f, 0.78f, 1.00f, 0.98f)
                       : (*bool_value_ ? ImVec4(0.56f, 0.88f, 1.00f, 0.94f)
                                       : ImVec4(0.42f, 0.54f, 0.68f, 0.82f)));
        draw_list->AddRectFilled(widget_min, ImVec2(widget_min.x + widget_size.x, widget_min.y + widget_size.y),
                                 fill_color, 14.0f);
        draw_list->AddRect(widget_min, ImVec2(widget_min.x + widget_size.x, widget_min.y + widget_size.y),
                           border_color, 14.0f, 0, is_editing ? 2.2f : 1.6f);

        const char* mode_text = shortcut_mode_ == MenuShortcutMode::Hold ? "HOLD" : "TOGGLE";
        const char* state_text = *bool_value_ ? "ON" : "OFF";
        ImVec2 label_size = ImGui::CalcTextSize(label_.c_str());
        ImVec2 meta_size = ImGui::CalcTextSize(mode_text);
        ImVec2 state_size = ImGui::CalcTextSize(state_text);
        draw_list->AddText(ImVec2(widget_min.x + 14.0f, widget_min.y + 8.0f),
                           ImGui::GetColorU32(ImVec4(0.94f, 0.98f, 1.00f, 1.00f)), label_.c_str());
        draw_list->AddText(ImVec2(widget_min.x + 14.0f, widget_min.y + widget_size.y - meta_size.y - 8.0f),
                           ImGui::GetColorU32(ImVec4(0.78f, 0.88f, 0.98f, 0.96f)), mode_text);
        draw_list->AddText(
            ImVec2(widget_min.x + widget_size.x - state_size.x - 14.0f,
                   widget_min.y + (widget_size.y - state_size.y) * 0.5f),
            ImGui::GetColorU32(ImVec4(1.00f, 1.00f, 1.00f, 0.98f)),
            state_text);

        if (is_editing) {
            const float grip_size = std::clamp(std::min(widget_size.x, widget_size.y) * 0.24f, 14.0f, 24.0f);
            const ImVec2 grip_pos(widget_size.x - grip_size - 4.0f, widget_size.y - grip_size - 4.0f);
            ImGui::SetCursorPos(grip_pos);
            ImGui::InvisibleButton("##resize_grip", ImVec2(grip_size, grip_size));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                shortcut_size_.x += io.MouseDelta.x;
                shortcut_size_.y += io.MouseDelta.y;
                shortcut_size_ = ClampShortcutSize(shortcut_size_);
                value_changed = true;
            }

            const ImVec2 grip_min = ImGui::GetItemRectMin();
            const ImVec2 grip_max = ImGui::GetItemRectMax();
            draw_list->AddRectFilled(grip_min, grip_max, ImGui::GetColorU32(ImVec4(0.18f, 0.62f, 1.00f, 0.34f)), 6.0f);
            draw_list->AddLine(ImVec2(grip_min.x + 4.0f, grip_max.y - 8.0f), ImVec2(grip_max.x - 4.0f, grip_min.y + 8.0f),
                               ImGui::GetColorU32(ImVec4(0.86f, 0.94f, 1.00f, 0.92f)), 1.6f);
            const ImVec2 window_pos = ImGui::GetWindowPos();
            draw_list->AddRect(window_pos, ImVec2(window_pos.x + widget_size.x, window_pos.y + widget_size.y),
                               ImGui::GetColorU32(ImVec4(0.26f, 0.74f, 1.00f, 0.92f)), 14.0f, 0, 2.0f);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !shortcut_touch_active_ &&
        shortcut_mode_ == MenuShortcutMode::Hold && shortcut_hold_active_) {
        shortcut_hold_active_ = false;
        if (*bool_value_ != shortcut_hold_restore_value_) {
            *bool_value_ = shortcut_hold_restore_value_;
            value_changed = true;
        }
    }

    return value_changed;
}

MenuSettingSpec::MenuSettingSpec(MenuSettingKind kind, std::string key, std::string label)
    : kind_(kind), key_(std::move(key)), label_(std::move(label)) {}

MenuSettingSpec& MenuSettingSpec::Tooltip(std::string value) {
    tooltip_ = std::move(value);
    return *this;
}

MenuSettingSpec& MenuSettingSpec::Persisted(bool value) {
    persisted_ = value;
    return *this;
}

MenuSettingSpec& MenuSettingSpec::VisibleIf(std::function<bool()> predicate) {
    visible_if_ = std::move(predicate);
    return *this;
}

MenuSettingSpec& MenuSettingSpec::OnChange(std::function<void()> callback) {
    on_change_ = std::move(callback);
    return *this;
}

MenuSettingSpec& MenuSettingSpec::ShortcutSupported(bool value) {
    shortcut_supported_ = value && kind_ == MenuSettingKind::Bool;
    if (!shortcut_supported_) {
        shortcut_enabled_ = false;
        shortcut_editor_open_ = false;
        shortcut_touch_active_ = false;
        if (g_active_shortcut_editor == this) {
            g_active_shortcut_editor = nullptr;
        }
        shortcut_hold_active_ = false;
    }
    return *this;
}

bool MenuSettingSpec::IsVisible() const {
    return !visible_if_ || visible_if_();
}

MenuSectionSpec::MenuSectionSpec(std::string id, std::string title, MenuColumn column)
    : id_(std::move(id)), title_(std::move(title)), column_(column) {}

MenuSettingSpec& MenuSectionSpec::AddBool(const std::string& key, const std::string& label, bool* value) {
    settings_.emplace_back(MenuSettingKind::Bool, key, label);
    auto& setting = settings_.back();
    setting.bool_value_ = value;
    setting.default_bool_ = value ? *value : false;
    setting.shortcut_supported_ = true;
    setting.default_shortcut_enabled_ = setting.shortcut_enabled_;
    setting.default_shortcut_mode_ = setting.shortcut_mode_;
    setting.default_shortcut_pos_ = setting.shortcut_pos_;
    setting.default_shortcut_size_ = setting.shortcut_size_;
    return setting;
}

MenuSettingSpec& MenuSectionSpec::AddInt(const std::string& key, const std::string& label, int* value,
                                         int min_value, int max_value, const char* format) {
    settings_.emplace_back(MenuSettingKind::Int, key, label);
    auto& setting = settings_.back();
    setting.int_value_ = value;
    setting.int_min_ = min_value;
    setting.int_max_ = max_value;
    setting.format_ = format ? format : "%d";
    setting.default_int_ = value ? *value : 0;
    return setting;
}

MenuSettingSpec& MenuSectionSpec::AddFloat(const std::string& key, const std::string& label, float* value,
                                           float min_value, float max_value, const char* format) {
    settings_.emplace_back(MenuSettingKind::Float, key, label);
    auto& setting = settings_.back();
    setting.float_value_ = value;
    setting.float_min_ = min_value;
    setting.float_max_ = max_value;
    setting.format_ = format ? format : "%.2f";
    setting.default_float_ = value ? *value : 0.0f;
    return setting;
}

MenuSettingSpec& MenuSectionSpec::AddChoice(const std::string& key, const std::string& label, int* value,
                                            std::vector<MenuChoiceOption> choices) {
    settings_.emplace_back(MenuSettingKind::Choice, key, label);
    auto& setting = settings_.back();
    setting.int_value_ = value;
    setting.choices_ = std::move(choices);
    setting.default_int_ = value ? *value : 0;
    return setting;
}

MenuSettingSpec& MenuSectionSpec::AddButton(const std::string& key, const std::string& label,
                                            std::function<void()> action) {
    settings_.emplace_back(MenuSettingKind::Button, key, label);
    auto& setting = settings_.back();
    setting.button_action_ = std::move(action);
    setting.persisted_ = false;
    return setting;
}

MenuSettingSpec& MenuSectionSpec::AddText(const std::string& key, const std::string& label,
                                          std::function<std::string()> provider) {
    settings_.emplace_back(MenuSettingKind::Text, key, label);
    auto& setting = settings_.back();
    setting.text_provider_ = std::move(provider);
    setting.persisted_ = false;
    return setting;
}

MenuSectionSpec& MenuSectionSpec::VisibleIf(std::function<bool()> predicate) {
    visible_if_ = std::move(predicate);
    return *this;
}

MenuSectionSpec& MenuSectionSpec::FooterDraw(std::function<void()> callback) {
    footer_draw_ = std::move(callback);
    return *this;
}

bool MenuSectionSpec::IsVisible() const {
    return !visible_if_ || visible_if_();
}

MenuPageSpec::MenuPageSpec(std::string id, std::string title)
    : id_(std::move(id)), title_(std::move(title)) {}

MenuSectionSpec& MenuPageSpec::AddSection(const std::string& id, const std::string& title, MenuColumn column) {
    sections_.emplace_back(id, title, column);
    return sections_.back();
}

MenuRegistry& MenuRegistry::Instance() {
    static MenuRegistry instance;
    return instance;
}

void MenuRegistry::Reset() {
    pages_.clear();
}

MenuPageSpec& MenuRegistry::AddPage(const std::string& id, const std::string& title) {
    pages_.emplace_back(id, title);
    return pages_.back();
}

MenuPageSpec* MenuRegistry::FindPage(const std::string& id) {
    for (auto& page : pages_) {
        if (page.id_ == id) return &page;
    }
    return nullptr;
}

const MenuPageSpec* MenuRegistry::FindPage(const std::string& id) const {
    for (const auto& page : pages_) {
        if (page.id_ == id) return &page;
    }
    return nullptr;
}

void MenuRegistry::RenderPage(const std::string& id) {
    const MenuPageSpec* page = FindPage(id);
    if (!page) {
        ImGui::TextDisabled("Page '%s' not registered", id.c_str());
        return;
    }

    if (!ImGui::BeginTable((id + "_layout").c_str(), 2,
                           ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        return;
    }

    for (int column_index = 0; column_index < 2; ++column_index) {
        ImGui::TableNextColumn();
        MenuColumn column = (column_index == 0) ? MenuColumn::Left : MenuColumn::Right;
        for (const auto& section : page->sections_) {
            if (section.column_ != column || !section.IsVisible()) continue;

            ImVec2 section_start;
            BeginSectionFrame(section.title_.c_str(), section_start);
            for (auto& setting : const_cast<std::vector<MenuSettingSpec>&>(section.settings_)) {
                if (!setting.IsVisible()) continue;

                bool changed = false;
                const bool disable_setting = IsShortcutEditorActive() && g_active_shortcut_editor != &setting;
                if (disable_setting) {
                    ImGui::BeginDisabled(true);
                }
                switch (setting.kind_) {
                    case MenuSettingKind::Bool:
                        if (setting.bool_value_) {
                            changed = ImGui::Checkbox(setting.label_.c_str(), setting.bool_value_);
                            DrawTooltip(setting.tooltip_);
                            if (setting.DrawShortcutEditor()) {
                                changed = true;
                            }
                        }
                        break;
                    case MenuSettingKind::Int:
                        if (setting.int_value_) {
                            changed = DrawProgressSliderInt(setting.label_.c_str(), setting.int_value_,
                                                            setting.int_min_, setting.int_max_,
                                                            setting.format_.c_str());
                            DrawTooltip(setting.tooltip_);
                        }
                        break;
                    case MenuSettingKind::Float:
                        if (setting.float_value_) {
                            changed = DrawProgressSliderFloat(setting.label_.c_str(), setting.float_value_,
                                                              setting.float_min_, setting.float_max_,
                                                              setting.format_.c_str());
                            DrawTooltip(setting.tooltip_);
                        }
                        break;
                    case MenuSettingKind::Choice:
                        if (setting.int_value_) {
                            std::vector<const char*> labels;
                            labels.reserve(setting.choices_.size());
                            int current_index = 0;
                            for (size_t i = 0; i < setting.choices_.size(); ++i) {
                                labels.push_back(setting.choices_[i].label.c_str());
                                if (setting.choices_[i].value == *setting.int_value_) {
                                    current_index = static_cast<int>(i);
                                }
                            }
                            if (!labels.empty()) {
                                ImGui::TextUnformatted(setting.label_.c_str());
                                changed = ImGui::Combo((std::string("##") + setting.label_).c_str(),
                                                       &current_index, labels.data(),
                                                       static_cast<int>(labels.size()));
                                if (changed) {
                                    *setting.int_value_ = setting.choices_[current_index].value;
                                }
                                DrawTooltip(setting.tooltip_);
                            }
                        }
                        break;
                    case MenuSettingKind::Button:
                        changed = ImGui::Button(setting.label_.c_str());
                        if (changed && setting.button_action_) {
                            setting.button_action_();
                        }
                        DrawTooltip(setting.tooltip_);
                        changed = false;
                        break;
                    case MenuSettingKind::Text:
                        if (!setting.label_.empty()) {
                            ImGui::Text("%s", setting.label_.c_str());
                        }
                        if (setting.text_provider_) {
                            ImGui::TextWrapped("%s", setting.text_provider_().c_str());
                        }
                        DrawTooltip(setting.tooltip_);
                        break;
                }
                if (disable_setting) {
                    ImGui::EndDisabled();
                }

                if (changed && setting.on_change_) {
                    setting.on_change_();
                }
            }

            if (section.footer_draw_) {
                section.footer_draw_();
            }
            EndSectionFrame(section_start);
        }
    }

    ImGui::EndTable();
}

void MenuRegistry::RenderShortcutWidgets() {
    if (IsShortcutEditorActive()) {
        ImDrawList* overlay = ImGui::GetForegroundDrawList();
        ImGuiIO& io = ImGui::GetIO();
        overlay->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, ImGui::GetColorU32(ImVec4(0.10f, 0.10f, 0.12f, 0.52f)));
    }

    size_t order_index = 0;
    for (auto& page : pages_) {
        for (auto& section : page.sections_) {
            for (auto& setting : section.settings_) {
                if (!setting.shortcut_supported_ || !setting.shortcut_enabled_) continue;
                if (IsShortcutEditorActive() && g_active_shortcut_editor != &setting) {
                    ++order_index;
                    continue;
                }
                if (setting.RenderShortcutWidget(order_index) && setting.on_change_) {
                    setting.on_change_();
                }
                ++order_index;
            }
        }
    }
}

void MenuRegistry::SetDefaultConfigPath(std::filesystem::path path) {
    default_config_path_ = std::move(path);
}

const std::filesystem::path& MenuRegistry::GetDefaultConfigPath() const {
    return default_config_path_;
}

MenuConfigResult MenuRegistry::ExportToFile(const std::filesystem::path& path) const {
    MenuConfigResult result;
    std::ostringstream out;
    out << "{\n";
    bool first_page = true;
    for (const auto& page : pages_) {
        bool page_has_persisted = false;
        for (const auto& section : page.sections_) {
            for (const auto& setting : section.settings_) {
                if (setting.persisted_) {
                    page_has_persisted = true;
                    break;
                }
            }
            if (page_has_persisted) break;
        }
        if (!page_has_persisted) continue;

        if (!first_page) out << ",\n";
        first_page = false;
        out << "  \"" << EscapeJsonString(page.id_) << "\": {\n";
        bool first_section = true;
        for (const auto& section : page.sections_) {
            bool section_has_persisted = false;
            for (const auto& setting : section.settings_) {
                if (setting.persisted_) {
                    section_has_persisted = true;
                    break;
                }
            }
            if (!section_has_persisted) continue;

            if (!first_section) out << ",\n";
            first_section = false;
            out << "    \"" << EscapeJsonString(section.id_) << "\": {\n";
            bool first_setting = true;
            for (const auto& setting : section.settings_) {
                if (!setting.persisted_) continue;
                if (!first_setting) out << ",\n";
                first_setting = false;
                out << "      \"" << EscapeJsonString(setting.key_) << "\": ";
                if (setting.kind_ == MenuSettingKind::Bool && setting.shortcut_supported_) {
                    out << "{ "
                        << "\"value\": " << ((*setting.bool_value_) ? "true" : "false")
                        << ", \"shortcut_enabled\": " << (setting.shortcut_enabled_ ? "true" : "false")
                        << ", \"shortcut_mode\": " << static_cast<int>(setting.shortcut_mode_)
                        << ", \"shortcut_pos_x\": " << setting.shortcut_pos_.x
                        << ", \"shortcut_pos_y\": " << setting.shortcut_pos_.y
                        << ", \"shortcut_size_x\": " << setting.shortcut_size_.x
                        << ", \"shortcut_size_y\": " << setting.shortcut_size_.y
                        << " }";
                } else {
                    switch (setting.kind_) {
                        case MenuSettingKind::Bool:
                            out << ((*setting.bool_value_) ? "true" : "false");
                            break;
                        case MenuSettingKind::Int:
                        case MenuSettingKind::Choice:
                            out << *setting.int_value_;
                            break;
                        case MenuSettingKind::Float:
                            out << *setting.float_value_;
                            break;
                        case MenuSettingKind::Button:
                        case MenuSettingKind::Text:
                            out << "null";
                            break;
                    }
                }
                ++result.applied;
            }
            out << "\n    }";
        }
        out << "\n  }";
    }
    out << "\n}\n";

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream file(path);
    if (!file) {
        result.message = std::string("导出失败: ") + std::strerror(errno);
        return result;
    }
    file << out.str();
    result.success = true;
    result.message = std::string("导出成功: ") + path.string();
    return result;
}

MenuConfigResult MenuRegistry::ImportFromFile(const std::filesystem::path& path) {
    MenuConfigResult result;
    std::ifstream file(path);
    if (!file) {
        result.message = std::string("导入失败: 无法打开 ") + path.string();
        return result;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    JsonValue root;
    std::string parse_error;
    JsonParser parser(buffer.str());
    if (!parser.Parse(root, parse_error)) {
        result.message = std::string("导入失败: JSON 解析错误 - ") + parse_error;
        return result;
    }
    if (root.type != JsonValue::Type::Object) {
        result.message = "导入失败: 顶层必须是对象";
        return result;
    }

    for (auto& page : pages_) {
        const JsonValue* page_value = nullptr;
        if (!GetObjectMember(root, page.id_, page_value)) continue;
        for (auto& section : page.sections_) {
            const JsonValue* section_value = nullptr;
            if (!GetObjectMember(*page_value, section.id_, section_value)) continue;
            for (auto& setting : section.settings_) {
                if (!setting.persisted_) continue;
                const JsonValue* setting_value = nullptr;
                if (!GetObjectMember(*section_value, setting.key_, setting_value)) continue;

                bool changed = false;
                switch (setting.kind_) {
                    case MenuSettingKind::Bool:
                        if (!setting.bool_value_) {
                            ++result.errors;
                            break;
                        }
                        if (setting.shortcut_supported_ && setting_value->type == JsonValue::Type::Object) {
                            const JsonValue* bool_value = nullptr;
                            if (GetObjectMember(*setting_value, "value", bool_value) &&
                                bool_value->type == JsonValue::Type::Bool) {
                                *setting.bool_value_ = bool_value->bool_value;
                                changed = true;
                            } else {
                                ++result.errors;
                            }

                            const JsonValue* shortcut_enabled = nullptr;
                            if (GetObjectMember(*setting_value, "shortcut_enabled", shortcut_enabled)) {
                                if (shortcut_enabled->type == JsonValue::Type::Bool) {
                                    setting.shortcut_enabled_ = shortcut_enabled->bool_value;
                                } else {
                                    ++result.errors;
                                }
                            }

                            const JsonValue* shortcut_mode = nullptr;
                            if (GetObjectMember(*setting_value, "shortcut_mode", shortcut_mode)) {
                                if (shortcut_mode->type == JsonValue::Type::Number) {
                                    const int mode_value = static_cast<int>(std::llround(shortcut_mode->number_value));
                                    if (mode_value == static_cast<int>(MenuShortcutMode::Hold) ||
                                        mode_value == static_cast<int>(MenuShortcutMode::Toggle)) {
                                        setting.shortcut_mode_ = static_cast<MenuShortcutMode>(mode_value);
                                    } else {
                                        ++result.errors;
                                    }
                                } else {
                                    ++result.errors;
                                }
                            }

                            const JsonValue* shortcut_pos_x = nullptr;
                            if (GetObjectMember(*setting_value, "shortcut_pos_x", shortcut_pos_x)) {
                                if (shortcut_pos_x->type == JsonValue::Type::Number) {
                                    setting.shortcut_pos_.x = static_cast<float>(shortcut_pos_x->number_value);
                                } else {
                                    ++result.errors;
                                }
                            }

                            const JsonValue* shortcut_pos_y = nullptr;
                            if (GetObjectMember(*setting_value, "shortcut_pos_y", shortcut_pos_y)) {
                                if (shortcut_pos_y->type == JsonValue::Type::Number) {
                                    setting.shortcut_pos_.y = static_cast<float>(shortcut_pos_y->number_value);
                                } else {
                                    ++result.errors;
                                }
                            }

                            const JsonValue* shortcut_size_x = nullptr;
                            if (GetObjectMember(*setting_value, "shortcut_size_x", shortcut_size_x)) {
                                if (shortcut_size_x->type == JsonValue::Type::Number) {
                                    setting.shortcut_size_.x = static_cast<float>(shortcut_size_x->number_value);
                                } else {
                                    ++result.errors;
                                }
                            }

                            const JsonValue* shortcut_size_y = nullptr;
                            if (GetObjectMember(*setting_value, "shortcut_size_y", shortcut_size_y)) {
                                if (shortcut_size_y->type == JsonValue::Type::Number) {
                                    setting.shortcut_size_.y = static_cast<float>(shortcut_size_y->number_value);
                                } else {
                                    ++result.errors;
                                }
                            }
                            setting.shortcut_size_ = ClampShortcutSize(setting.shortcut_size_);
                        } else if (setting_value->type == JsonValue::Type::Bool) {
                            *setting.bool_value_ = setting_value->bool_value;
                            changed = true;
                        } else {
                            ++result.errors;
                        }
                        break;
                    case MenuSettingKind::Int:
                    case MenuSettingKind::Choice:
                        if (setting_value->type == JsonValue::Type::Number && setting.int_value_) {
                            int new_value = static_cast<int>(std::llround(setting_value->number_value));
                            if (setting.kind_ == MenuSettingKind::Int) {
                                new_value = std::clamp(new_value, setting.int_min_, setting.int_max_);
                            }
                            *setting.int_value_ = new_value;
                            changed = true;
                        } else {
                            ++result.errors;
                        }
                        break;
                    case MenuSettingKind::Float:
                        if (setting_value->type == JsonValue::Type::Number && setting.float_value_) {
                            float new_value = static_cast<float>(setting_value->number_value);
                            new_value = std::clamp(new_value, setting.float_min_, setting.float_max_);
                            *setting.float_value_ = new_value;
                            changed = true;
                        } else {
                            ++result.errors;
                        }
                        break;
                    case MenuSettingKind::Button:
                    case MenuSettingKind::Text:
                        break;
                }

                if (changed) {
                    ++result.applied;
                    if (setting.on_change_) {
                        setting.on_change_();
                    }
                }
            }
        }
    }

    result.success = (result.errors == 0);
    if (result.success) {
        result.message = std::string("导入成功: ") + path.string();
    } else {
        result.message = std::string("导入完成，") + std::to_string(result.errors) + " 项无效";
    }
    return result;
}

MenuConfigResult MenuRegistry::ResetToDefaults() {
    MenuConfigResult result;
    for (auto& page : pages_) {
        for (auto& section : page.sections_) {
            for (auto& setting : section.settings_) {
                if (!setting.persisted_) continue;
                switch (setting.kind_) {
                    case MenuSettingKind::Bool:
                        if (setting.bool_value_) {
                            *setting.bool_value_ = setting.default_bool_;
                            if (setting.shortcut_supported_) {
                                setting.shortcut_enabled_ = setting.default_shortcut_enabled_;
                                setting.shortcut_mode_ = setting.default_shortcut_mode_;
                                setting.shortcut_pos_ = setting.default_shortcut_pos_;
                                setting.shortcut_size_ = setting.default_shortcut_size_;
                                setting.shortcut_editor_open_ = false;
                                setting.shortcut_touch_active_ = false;
                                if (g_active_shortcut_editor == &setting) {
                                    g_active_shortcut_editor = nullptr;
                                }
                                setting.shortcut_hold_active_ = false;
                            }
                            ++result.applied;
                        }
                        break;
                    case MenuSettingKind::Int:
                    case MenuSettingKind::Choice:
                        if (setting.int_value_) {
                            *setting.int_value_ = setting.default_int_;
                            ++result.applied;
                        }
                        break;
                    case MenuSettingKind::Float:
                        if (setting.float_value_) {
                            *setting.float_value_ = setting.default_float_;
                            ++result.applied;
                        }
                        break;
                    case MenuSettingKind::Button:
                    case MenuSettingKind::Text:
                        break;
                }
                if (setting.on_change_) {
                    setting.on_change_();
                }
            }
        }
    }
    result.success = true;
    result.message = "已恢复默认配置";
    return result;
}
