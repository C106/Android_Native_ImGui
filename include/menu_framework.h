#pragma once

#include "imgui.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

enum class MenuColumn {
    Left,
    Right,
};

enum class MenuSettingKind {
    Bool,
    Int,
    Float,
    Choice,
    MultiBoolChoice,
    Button,
    Text,
};

enum class MenuShortcutMode {
    Hold = 0,
    Toggle = 1,
};

enum class MenuRenderMode {
    Normal,
    DebugOnly,
};

struct MenuChoiceOption {
    std::string label;
    int value = 0;
};

struct MenuMultiBoolOption {
    std::string key;
    std::string label;
    bool* value = nullptr;
    bool default_value = false;
};

struct MenuConfigResult {
    bool success = false;
    size_t applied = 0;
    size_t errors = 0;
    std::string message;
};

class MenuSettingSpec {
public:
    explicit MenuSettingSpec(MenuSettingKind kind, std::string key, std::string label);

    MenuSettingSpec& Tooltip(std::string value);
    MenuSettingSpec& Persisted(bool value);
    MenuSettingSpec& VisibleIf(std::function<bool()> predicate);
    MenuSettingSpec& OnChange(std::function<void()> callback);
    MenuSettingSpec& ShortcutSupported(bool value);
    MenuSettingSpec& DebugOnly(bool value = true);

private:
    friend class MenuSectionSpec;
    friend class MenuRegistry;

    bool IsVisible() const;
    bool DrawShortcutEditor();
    bool RenderShortcutWidget(size_t order_index);

    MenuSettingKind kind_;
    std::string key_;
    std::string label_;
    std::string tooltip_;
    bool persisted_ = true;
    bool debug_only_ = false;
    std::function<bool()> visible_if_;
    std::function<void()> on_change_;

    bool* bool_value_ = nullptr;
    int* int_value_ = nullptr;
    float* float_value_ = nullptr;
    bool shortcut_supported_ = false;
    bool shortcut_enabled_ = false;
    MenuShortcutMode shortcut_mode_ = MenuShortcutMode::Toggle;
    bool default_shortcut_enabled_ = false;
    MenuShortcutMode default_shortcut_mode_ = MenuShortcutMode::Toggle;
    ImVec2 shortcut_pos_ = ImVec2(-1.0f, -1.0f);
    ImVec2 default_shortcut_pos_ = ImVec2(-1.0f, -1.0f);
    ImVec2 shortcut_size_ = ImVec2(188.0f, 44.0f);
    ImVec2 default_shortcut_size_ = ImVec2(188.0f, 44.0f);
    bool shortcut_editor_open_ = false;
    bool shortcut_touch_active_ = false;
    bool shortcut_hold_active_ = false;
    bool shortcut_hold_restore_value_ = false;

    int int_min_ = 0;
    int int_max_ = 0;
    float float_min_ = 0.0f;
    float float_max_ = 0.0f;
    std::string format_ = "%d";
    std::vector<MenuChoiceOption> choices_;
    std::vector<MenuMultiBoolOption> multi_bool_choices_;
    std::function<void()> button_action_;
    std::function<std::string()> text_provider_;

    bool default_bool_ = false;
    int default_int_ = 0;
    float default_float_ = 0.0f;
};

class MenuSectionSpec {
public:
    MenuSectionSpec(std::string id, std::string title, MenuColumn column);

    MenuSettingSpec& AddBool(const std::string& key, const std::string& label, bool* value);
    MenuSettingSpec& AddInt(const std::string& key, const std::string& label, int* value,
                            int min_value, int max_value, const char* format = "%d");
    MenuSettingSpec& AddFloat(const std::string& key, const std::string& label, float* value,
                              float min_value, float max_value, const char* format = "%.2f");
    MenuSettingSpec& AddChoice(const std::string& key, const std::string& label, int* value,
                               std::vector<MenuChoiceOption> choices);
    MenuSettingSpec& AddMultiBoolChoice(const std::string& key, const std::string& label,
                                        std::vector<MenuMultiBoolOption> options);
    MenuSettingSpec& AddButton(const std::string& key, const std::string& label,
                               std::function<void()> action);
    MenuSettingSpec& AddText(const std::string& key, const std::string& label,
                             std::function<std::string()> provider);

    MenuSectionSpec& VisibleIf(std::function<bool()> predicate);
    MenuSectionSpec& FooterDraw(std::function<void()> callback);
    MenuSectionSpec& DebugOnly(bool value = true);

private:
    friend class MenuPageSpec;
    friend class MenuRegistry;

    bool IsVisible() const;

    std::string id_;
    std::string title_;
    MenuColumn column_ = MenuColumn::Left;
    bool debug_only_ = false;
    std::vector<MenuSettingSpec> settings_;
    std::function<bool()> visible_if_;
    std::function<void()> footer_draw_;
};

class MenuPageSpec {
public:
    MenuPageSpec(std::string id, std::string title);

    MenuSectionSpec& AddSection(const std::string& id, const std::string& title, MenuColumn column);

private:
    friend class MenuRegistry;

    std::string id_;
    std::string title_;
    std::vector<MenuSectionSpec> sections_;
};

class MenuRegistry {
public:
    static MenuRegistry& Instance();

    void Reset();
    MenuPageSpec& AddPage(const std::string& id, const std::string& title);
    void RenderShortcutWidgets();
    void RenderPage(const std::string& id, MenuRenderMode mode = MenuRenderMode::Normal);

    void SetDefaultConfigPath(std::filesystem::path path);
    const std::filesystem::path& GetDefaultConfigPath() const;

    MenuConfigResult ExportToFile(const std::filesystem::path& path) const;
    MenuConfigResult ImportFromFile(const std::filesystem::path& path);
    MenuConfigResult ResetToDefaults();

private:
    MenuRegistry() = default;

    MenuPageSpec* FindPage(const std::string& id);
    const MenuPageSpec* FindPage(const std::string& id) const;

    std::vector<MenuPageSpec> pages_;
    std::filesystem::path default_config_path_ = "debugger_config.json";
};
