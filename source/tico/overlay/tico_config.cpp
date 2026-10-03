// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "tico/overlay/tico_config.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>

#include <json.hpp>

extern "C" {
#include "prefs.h"
#include "tico/tico_entry.h"
}

namespace SwitchFrontend::TicoConfig {
namespace {

using OptionMap = std::map<std::string, std::string, std::less<>>;

constexpr std::array<const char*, 3> kConfigPaths = {{
    "sdmc:/tico/config/cores/drastic.jsonc",
    "sdmc:/tico/config/cores/drastic.json",
    "romfs:/config/drastic.jsonc",
}};

constexpr const char* kDefaultWritableConfigPath = "sdmc:/tico/config/cores/drastic.jsonc";

void EnsureWritableConfigDirectory() {
    mkdir("sdmc:/tico", 0777);
    mkdir("sdmc:/tico/config", 0777);
    mkdir("sdmc:/tico/config/cores", 0777);
}

// Strips // line and /* */ block comments so a .jsonc file parses as plain JSON.
std::string StripJsonComments(std::string_view input) {
    std::string output;
    output.reserve(input.size());

    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (in_string) {
            output.push_back(c);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            output.push_back(c);
            continue;
        }
        if (c == '/' && i + 1 < input.size()) {
            if (input[i + 1] == '/') {
                i += 2;
                while (i < input.size() && input[i] != '\n') {
                    ++i;
                }
                if (i < input.size()) {
                    output.push_back('\n');
                }
                continue;
            }
            if (input[i + 1] == '*') {
                i += 2;
                while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) {
                    ++i;
                }
                if (i + 1 < input.size()) {
                    ++i;
                }
                continue;
            }
        }
        output.push_back(c);
    }
    return output;
}

bool ReadWholeFile(const char* path, std::string& out) {
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(fp);
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    out.resize(read);
    return true;
}

// Converts a JSON scalar into the canonical string we store internally.
std::string JsonScalarToString(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    if (value.is_number_float()) {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", value.get<double>());
        return text;
    }
    return {};
}

std::string LowerCopy(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::optional<bool> ParseBool(std::string_view value) {
    const std::string lower = LowerCopy(value);
    if (lower == "true" || lower == "1" || lower == "on" || lower == "yes" ||
        lower == "enabled") {
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "off" || lower == "no" ||
        lower == "disabled") {
        return false;
    }
    return std::nullopt;
}

std::optional<int> ParseInt(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        const int result = std::stoi(std::string(value), &consumed);
        if (consumed == value.size()) {
            return result;
        }
    } catch (...) {
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Option catalogue. Keys, values and labels follow the DrasticDS launcher's
// settings, so a setting means the same thing in drastic.ini and here.

#define CHOICES(name) name, sizeof(name) / sizeof(name[0])
#define NO_CHOICES nullptr, 0

constexpr OptionChoice kRendererChoices[] = {
    {"vk", nullptr, "Vulkan (NVK)"},
    {"gl", nullptr, "OpenGL (NVC0)"},
    {"zink", nullptr, "Zink (OpenGL on NVK)"},
};
constexpr OptionChoice kLayoutChoices[] = {
    {"vertical", nullptr, "Vertical"},
    {"horizontal", nullptr, "Horizontal"},
    {"top", nullptr, "Top screen only"},
    {"bottom", nullptr, "Touch screen only"},
    {"hybrid_top", nullptr, "Hybrid (top large)"},
    {"hybrid_bottom", nullptr, "Hybrid (touch large)"},
    {"custom", nullptr, "Custom"},
};
constexpr OptionChoice kRotationChoices[] = {
    {"0", nullptr, "0 degrees"},
    {"1", nullptr, "90 degrees"},
    {"2", nullptr, "180 degrees"},
    {"3", nullptr, "270 degrees"},
};
constexpr OptionChoice kFilterChoices[] = {
    {"nearest", nullptr, "Nearest"},
    {"linear", nullptr, "Linear"},
    {"quilez", nullptr, "Quilez smooth"},
    {"scanline", nullptr, "Scanline"},
    {"scale2x", nullptr, "Scale2x"},
    {"hq2x", nullptr, "HQ2x"},
    {"fxaa", nullptr, "FXAA"},
    {"fxaa_hq", nullptr, "FXAA high quality"},
    {"smaa", nullptr, "SMAA"},
    {"fsr", nullptr, "FSR 1.0"},
    {"custom", nullptr, "Custom shader"},
};
// replaced at run time by the shaders found on the SD card
constexpr OptionChoice kNoShaderChoices[] = {
    {"", nullptr, "None found"},
};
constexpr OptionChoice kHudPositionChoices[] = {
    {"hidden", "emulator_hidden", "Hidden"},
    {"top_left", "emulator_top_left", "Top Left"},
    {"top_right", "emulator_top_right", "Top Right"},
    {"bottom_left", "emulator_bottom_left", "Bottom Left"},
    {"bottom_right", "emulator_bottom_right", "Bottom Right"},
};
constexpr OptionChoice kLsfgFlowChoices[] = {
    {"0.25", nullptr, "Quarter (recommended)"},
    {"0.5", nullptr, "Half"},
};
constexpr OptionChoice kLatencyChoices[] = {
    {"0", nullptr, "Low"},
    {"1", nullptr, "Balanced"},
    {"2", nullptr, "High compatibility"},
    {"3", nullptr, "Maximum"},
};
constexpr OptionChoice kMicSourceChoices[] = {
    {"noise", nullptr, "Simulated noise"},
    {"external", nullptr, "External microphone"},
};
constexpr OptionChoice kMicLevelChoices[] = {
    {"0", nullptr, "Low"},
    {"1", nullptr, "Normal"},
    {"2", nullptr, "High"},
    {"3", nullptr, "Maximum"},
};
constexpr OptionChoice kThreadChoices[] = {
    {"1", nullptr, "1"},
    {"2", nullptr, "2"},
    {"3", nullptr, "3 (recommended)"},
};
constexpr OptionChoice kAutosaveChoices[] = {
    {"0", "emulator_off", "Off"},
    {"60", nullptr, "1 minute"},
    {"300", nullptr, "5 minutes"},
    {"600", nullptr, "10 minutes"},
    {"1800", nullptr, "30 minutes"},
};
// Drastic's native Slot-2 enum (build 109)
constexpr OptionChoice kSlot2Choices[] = {
    {"0", nullptr, "None"},
    {"1", nullptr, "GBA Cart"},
    {"2", nullptr, "SRAM Cart"},
    {"3", nullptr, "Rumble Pack"},
    {"4", nullptr, "Motion Pack (Official)"},
    {"5", nullptr, "Motion Pack (Homebrew)"},
};
constexpr OptionChoice kFrameskipTypeChoices[] = {
    {"0", nullptr, "Automatic"},
    {"1", nullptr, "Fixed"},
    {"2", nullptr, "Aggressive"},
    {"3", nullptr, "Maximum"},
};
constexpr OptionChoice kFastForwardChoices[] = {
    {"0", nullptr, "50%"},
    {"1", nullptr, "150%"},
    {"2", nullptr, "200%"},
    {"3", nullptr, "300%"},
    {"4", nullptr, "400%"},
    {"5", "emulator_unlimited", "Unlimited"},
};
constexpr OptionChoice kAutofireChoices[] = {
    {"0", nullptr, "Slow"},
    {"2", nullptr, "Normal"},
    {"4", nullptr, "Fast"},
    {"7", nullptr, "Very fast"},
};
constexpr OptionChoice kStylusChoices[] = {
    {"off", "emulator_off", "Off"},
    {"stick", nullptr, "Right stick"},
    {"motion", nullptr, "Motion controls"},
};
constexpr OptionChoice kHoldToggleChoices[] = {
    {"hold", nullptr, "Hold"},
    {"toggle", nullptr, "Toggle"},
};
constexpr OptionChoice kButtonChoices[] = {
    {"A", nullptr, "A"},         {"B", nullptr, "B"},
    {"X", nullptr, "X"},         {"Y", nullptr, "Y"},
    {"L", nullptr, "L"},         {"R", nullptr, "R"},
    {"ZL", nullptr, "ZL"},       {"ZR", nullptr, "ZR"},
    {"Plus", nullptr, "Plus"},   {"Minus", nullptr, "Minus"},
    {"StickL", nullptr, "L-Stick"}, {"StickR", nullptr, "R-Stick"},
    {"Up", nullptr, "D-Up"},     {"Down", nullptr, "D-Down"},
    {"Left", nullptr, "D-Left"}, {"Right", nullptr, "D-Right"},
    {"None", nullptr, "None"},
};
constexpr OptionChoice kFirmwareLanguageChoices[] = {
    {"-1", "emulator_auto", "Auto"},
    {"0", nullptr, "Japanese"},
    {"1", nullptr, "English"},
    {"2", nullptr, "French"},
    {"3", nullptr, "German"},
    {"4", nullptr, "Italian"},
    {"5", nullptr, "Spanish"},
    {"6", nullptr, "Korean"},
};
constexpr OptionChoice kFirmwareColorChoices[] = {
    {"0", nullptr, "Gray"},        {"1", nullptr, "Brown"},
    {"2", nullptr, "Red"},         {"3", nullptr, "Pink"},
    {"4", nullptr, "Orange"},      {"5", nullptr, "Yellow"},
    {"6", nullptr, "Lime green"},  {"7", nullptr, "Green"},
    {"8", nullptr, "Dark green"},  {"9", nullptr, "Sea green"},
    {"10", nullptr, "Turquoise"},  {"11", nullptr, "Blue"},
    {"12", nullptr, "Dark blue"},  {"13", nullptr, "Dark purple"},
    {"14", nullptr, "Violet"},     {"15", nullptr, "Magenta"},
};

#define TOGGLE(key, label, def, restart) \
    {key, nullptr, label, OptionType::Toggle, def, NO_CHOICES, 0, 0, 0, restart, 0}
#define CHOICE(key, label, choices, def, restart) \
    {key, nullptr, label, OptionType::Choice, def, CHOICES(choices), 0, 0, 0, restart, 0}
#define RANGE(key, label, min, max, step, def, restart) \
    {key, nullptr, label, OptionType::Range, def, NO_CHOICES, min, max, step, restart, 0}
#define TEXT(key, label, def, length) \
    {key, nullptr, label, OptionType::Text, def, NO_CHOICES, 0, 0, 0, true, length}

// Not constexpr: the custom shader choices are filled in at run time.
OptionDef kDisplayOptions[] = {
    CHOICE("Wrapper/Layout", "Screen layout", kLayoutChoices, "horizontal", false),
    TOGGLE("Wrapper/CustomAspectLock", "Lock native aspect ratio", "true", false),
    TOGGLE("Wrapper/SwapScreens", "Swap DS screens", "false", false),
    CHOICE("Wrapper/Rotation", "Rotation", kRotationChoices, "0", false),
    RANGE("Wrapper/ScreenGap", "Screen gap", 0, 128, 2, "8", false),
    TOGGLE("Wrapper/IntegerScale", "Integer scaling", "false", false),
    CHOICE("Wrapper/VideoFilter", "Drastic filter", kFilterChoices, "nearest", false),
    {"Wrapper/FsrSharpness", nullptr, "FSR sharpness %", OptionType::Range, "40", NO_CHOICES, 0,
     100, 20, false, 0, "Wrapper/VideoFilter", "fsr"},
    {"Wrapper/CustomShader", nullptr, "Custom shader", OptionType::Choice, "",
     CHOICES(kNoShaderChoices), 0, 0, 0, false, 0, "Wrapper/VideoFilter", "custom"},
    {"fps_counter_position", "emulator_fps_counter", "FPS Counter", OptionType::Choice, "hidden",
     CHOICES(kHudPositionChoices), 0, 0, 0, false, 0},
};
constexpr OptionDef kGraphicsOptions[] = {
    CHOICE("Wrapper/Renderer", "Renderer", kRendererChoices, "vk", true),
    TOGGLE("Wrapper/VulkanLowLatency", "Low-latency Vulkan", "false", true),
    TOGGLE("Drastic/Hires3D", "High-resolution 3D", "false", true),
    TOGGLE("Drastic/Threaded3D", "Threaded 3D", "true", false),
    TOGGLE("Drastic/DisableEdgeMarking", "Disable edge marking", "false", true),
    TOGGLE("Drastic/Use16BitColor", "16-bit color", "false", true),
    TOGGLE("Drastic/Blend", "Frame blending", "false", true),
    TOGGLE("Drastic/FixMainEngineScreen", "Fix main-engine screen", "false", true),
};
constexpr OptionDef kFrameGenerationOptions[] = {
    TOGGLE("Wrapper/LSFGEnabled", "LSFG 2x (Vulkan only)", "false", false),
    CHOICE("Wrapper/LSFGFlowScale", "Flow resolution", kLsfgFlowChoices, "0.25", true),
    TOGGLE("Wrapper/LSFGPerformance", "Performance mode", "true", true),
};
constexpr OptionDef kAudioOptions[] = {
    TOGGLE("Drastic/SoundEnabled", "Sound", "true", false),
    RANGE("Wrapper/Volume", "Volume", 0, 100, 5, "100", false),
    CHOICE("Drastic/AudioLatency", "Audio latency", kLatencyChoices, "2", true),
    TOGGLE("Drastic/MicEnabled", "Microphone", "true", false),
    CHOICE("Wrapper/MicrophoneSource", "Microphone source", kMicSourceChoices, "noise", false),
    CHOICE("Drastic/MicLevel", "Microphone level", kMicLevelChoices, "1", false),
};
constexpr OptionDef kEmulationOptions[] = {
    CHOICE("Drastic/CpuThreads", "CPU worker threads", kThreadChoices, "3", true),
    TOGGLE("Drastic/PreloadRoms", "Preload ROM", "true", true),
    TOGGLE("Drastic/AutoTrim", "Auto-trim ROM", "false", true),
    TOGGLE("Drastic/IgnoreGamecardLimit", "Ignore card size limit", "false", true),
    TOGGLE("Drastic/RtcSystemTime", "Always sync RTC", "true", true),
    CHOICE("Drastic/AutosaveInterval", "Autosave interval", kAutosaveChoices, "300", false),
    TOGGLE("Drastic/CheatsEnabled", "Cheats master switch", "true", false),
    TOGGLE("Drastic/LuaEnabled", "Lua scripts", "true", true),
    CHOICE("Drastic/Slot2Type", "Slot-2 accessory", kSlot2Choices, "1", true),
    TOGGLE("Drastic/BackupInSavestates", "Savestate backup data", "true", true),
    TOGGLE("Drastic/RawSaveFormat", "Raw save-file format", "false", true),
};
constexpr OptionDef kFrameRateOptions[] = {
    RANGE("Drastic/FrameskipValue", "Frames to skip", 0, 9, 1, "0", false),
    CHOICE("Drastic/FrameskipType", "Frame-skip method", kFrameskipTypeChoices, "0", false),
    TOGGLE("Drastic/FrameskipSafe", "Safe frame skipping", "false", false),
    CHOICE("Drastic/FastForwardSpeed", "Fast-forward speed", kFastForwardChoices, "2", false),
    CHOICE("Drastic/AutoFireSpeed", "Auto-fire speed", kAutofireChoices, "2", false),
};
constexpr OptionDef kControllerOptions[] = {
    TOGGLE("Wrapper/Vibration", "Rumble Pak vibration", "true", false),
    TOGGLE("Wrapper/Motion", "Gyro & accelerometer", "true", false),
    CHOICE("Wrapper/StylusMode", "Virtual stylus", kStylusChoices, "stick", false),
    CHOICE("Wrapper/AnalogTouchButton", "Stylus touch button", kButtonChoices, "StickR", true),
    RANGE("Wrapper/AnalogStylusSpeed", "Stick cursor speed", 1, 20, 1, "8", true),
    RANGE("Wrapper/MotionStylusSensitivity", "Motion sensitivity", 1, 20, 1, "10", false),
    TOGGLE("Wrapper/MouseStylus", "USB mouse stylus", "true", false),
    TOGGLE("Wrapper/AnalogDpad", "Analog stick as D-Pad", "true", true),
    RANGE("Wrapper/AnalogDeadzone", "Analog deadzone %", 5, 80, 5, "35", true),
    CHOICE("Wrapper/Pad/A", "DS A", kButtonChoices, "A", true),
    CHOICE("Wrapper/Pad/B", "DS B", kButtonChoices, "B", true),
    CHOICE("Wrapper/Pad/X", "DS X", kButtonChoices, "X", true),
    CHOICE("Wrapper/Pad/Y", "DS Y", kButtonChoices, "Y", true),
    CHOICE("Wrapper/Pad/L", "DS L", kButtonChoices, "L", true),
    CHOICE("Wrapper/Pad/R", "DS R", kButtonChoices, "R", true),
    CHOICE("Wrapper/Pad/Start", "DS Start", kButtonChoices, "Plus", true),
    CHOICE("Wrapper/Pad/Select", "DS Select", kButtonChoices, "Minus", true),
    CHOICE("Wrapper/Pad/Up", "D-Pad Up", kButtonChoices, "Up", true),
    CHOICE("Wrapper/Pad/Down", "D-Pad Down", kButtonChoices, "Down", true),
    CHOICE("Wrapper/Pad/Left", "D-Pad Left", kButtonChoices, "Left", true),
    CHOICE("Wrapper/Pad/Right", "D-Pad Right", kButtonChoices, "Right", true),
    CHOICE("Wrapper/FastForwardMode", "Fast-forward mode", kHoldToggleChoices, "hold", true),
};
constexpr OptionDef kHotkeyOptions[] = {
    TEXT("Wrapper/HotkeyMenu", "Quick menu", "Plus+Minus", 48),
    TEXT("Wrapper/HotkeyFastForward", "Fast-forward", "ZR", 48),
    TEXT("Wrapper/HotkeySwapScreens", "Swap screens", "ZL", 48),
    TEXT("Wrapper/HotkeyMicrophone", "Microphone", "StickL", 48),
    TEXT("Wrapper/HotkeyMotionStylusRecenter", "Recenter motion stylus", "L+R+StickR", 48),
    TEXT("Wrapper/HotkeyAutoFire", "Auto-fire modifier", "None", 48),
    TEXT("Wrapper/HotkeyLid", "Close/open lid", "None", 48),
    TEXT("Wrapper/HotkeySaveState", "Save state", "L+R+Minus+Y", 48),
    TEXT("Wrapper/HotkeyLoadState", "Load state", "L+R+Minus+X", 48),
    TEXT("Wrapper/HotkeyNextSlot", "Next state slot", "L+R+Minus+Up", 48),
    TEXT("Wrapper/HotkeyPreviousSlot", "Previous state slot", "L+R+Minus+Down", 48),
    TEXT("Wrapper/HotkeyReset", "Reset game", "L+R+Minus+A", 48),
    TEXT("Wrapper/HotkeyQuit", "Quit to tico", "None", 48),
};
constexpr OptionDef kFirmwareOptions[] = {
    TEXT("Drastic/FirmwareNickname", "Nickname", "Switch", 10),
    CHOICE("Drastic/FirmwareLanguage", "Language", kFirmwareLanguageChoices, "-1", true),
    CHOICE("Drastic/FirmwareColor", "Favorite color", kFirmwareColorChoices, "0", true),
    RANGE("Drastic/FirmwareBirthdayMonth", "Birthday month", 1, 12, 1, "6", true),
    RANGE("Drastic/FirmwareBirthdayDay", "Birthday day", 1, 31, 1, "6", true),
};

#define OPTIONS(name) name, sizeof(name) / sizeof(name[0])

const std::vector<OptionCategory> kCategories = {
    {"emulator_category_display", "Display", OPTIONS(kDisplayOptions)},
    {nullptr, "3D / Graphics", OPTIONS(kGraphicsOptions)},
    {nullptr, "Frame Generation", OPTIONS(kFrameGenerationOptions)},
    {"emulator_category_audio", "Audio", OPTIONS(kAudioOptions)},
    {"emulator_category_system", "Emulation", OPTIONS(kEmulationOptions)},
    {nullptr, "Frame Rate", OPTIONS(kFrameRateOptions)},
    {nullptr, "Controller", OPTIONS(kControllerOptions)},
    {nullptr, "Hotkeys", OPTIONS(kHotkeyOptions)},
    {"emulator_category_firmware", "Firmware", OPTIONS(kFirmwareOptions)},
};

// Index of the choice whose stored value matches, or 0 when none does.
std::size_t FindChoice(const OptionDef& option, std::string_view value) {
    const std::string lower = LowerCopy(value);
    for (std::size_t i = 0; i < option.choice_count; ++i) {
        if (LowerCopy(option.choices[i].value) == lower) {
            return i;
        }
    }
    return 0;
}

class Manager {
public:
    void ReloadConfig() {
        options.clear();
        loaded_path.clear();

        for (const char* path : kConfigPaths) {
            std::string content;
            if (!ReadWholeFile(path, content)) {
                continue;
            }
            const std::string stripped = StripJsonComments(content);
            nlohmann::json root = nlohmann::json::parse(stripped, nullptr, false);
            if (root.is_discarded() || !root.is_object()) {
                tico_log("tico config at %s is not a JSON object\n", path);
                continue;
            }
            for (auto it = root.begin(); it != root.end(); ++it) {
                if (it.value().is_object() || it.value().is_array()) {
                    continue;
                }
                options[it.key()] = JsonScalarToString(it.value());
            }
            loaded_path = path;
            tico_log("tico config loaded from %s (%zu options)\n", path, options.size());
            return;
        }
        tico_log("no tico config found; using defaults\n");
    }

    std::string GetConfigValue(std::string_view key, std::string_view default_value) const {
        const auto it = options.find(key);
        if (it != options.end()) {
            return it->second;
        }
        return std::string(default_value);
    }

    void SetConfigValue(const std::string& key, const std::string& value) {
        options[key] = value;
    }

    bool SaveConfig() {
        nlohmann::json root = nlohmann::json::object();
        for (const auto& [key, value] : options) {
            const OptionDef* option = FindOption(key);
            // text is kept as text even when it looks like a number
            if (option && option->type == OptionType::Text) {
                root[key] = value;
            } else if (const auto b = ParseBool(value); b && (!option || option->type == OptionType::Toggle)) {
                root[key] = *b;
            } else if (const auto i = ParseInt(value)) {
                root[key] = *i;
            } else {
                root[key] = value;
            }
        }
        const std::string serialized = root.dump(2);

        EnsureWritableConfigDirectory();

        const char* target = kDefaultWritableConfigPath;
        std::FILE* fp = std::fopen(target, "wb");
        if (!fp) {
            tico_log("failed to open tico config for write: %s\n", target);
            return false;
        }
        const std::size_t written = std::fwrite(serialized.data(), 1, serialized.size(), fp);
        std::fclose(fp);
        if (written != serialized.size()) {
            tico_log("failed to write full tico config: %s\n", target);
            return false;
        }
        loaded_path = target;
        return true;
    }

    std::string GetOptionValue(const OptionDef& option) const {
        return GetConfigValue(option.key, option.default_value);
    }

    bool GetBool(const OptionDef& option) const {
        if (const auto b = ParseBool(GetOptionValue(option))) {
            return *b;
        }
        return ParseBool(option.default_value).value_or(false);
    }

    int GetInt(const OptionDef& option) const {
        int value = ParseInt(GetOptionValue(option)).value_or(ParseInt(option.default_value).value_or(0));
        if (option.type == OptionType::Range) {
            value = std::clamp(value, option.min, option.max);
        }
        return value;
    }

    // For Choice options: the position of the stored value in the choice list.
    int GetChoiceIndex(const OptionDef& option) const {
        return static_cast<int>(FindChoice(option, GetOptionValue(option)));
    }

    const std::string& GetLoadedPath() const {
        return loaded_path;
    }

    std::size_t GetOptionCount() const {
        return options.size();
    }

private:
    OptionMap options;
    std::string loaded_path;
};

Manager& GetManager() {
    static Manager manager;
    return manager;
}

std::vector<OptionChoice> s_dynamic_choices;

// The value written to prefs: toggles as true/false, ranges clamped, and a
// choice that the file names but the list does not know replaced by a known one.
std::string PrefsValue(const OptionDef& option) {
    const Manager& config = GetManager();
    switch (option.type) {
    case OptionType::Toggle:
        return config.GetBool(option) ? "true" : "false";
    case OptionType::Range:
        return std::to_string(config.GetInt(option));
    case OptionType::Choice:
        if (option.choice_count == 0) {
            return config.GetOptionValue(option);
        }
        return option.choices[config.GetChoiceIndex(option)].value;
    case OptionType::Text:
    default:
        return config.GetOptionValue(option);
    }
}

void StoreInPrefs(const OptionDef& option) {
    // options without a section are the overlay's own, not the host's
    if (!std::strchr(option.key, '/')) {
        return;
    }
    prefs_set_string(option.key, PrefsValue(option).c_str());
}

} // namespace

void ReloadConfig() {
    GetManager().ReloadConfig();
}

std::string GetConfigValue(std::string_view key, std::string_view default_value) {
    return GetManager().GetConfigValue(key, default_value);
}

void SetConfigValue(const std::string& key, const std::string& value) {
    GetManager().SetConfigValue(key, value);
}

bool SaveConfig() {
    return GetManager().SaveConfig();
}

void ApplyToPrefs() {
    for (const OptionCategory& category : kCategories) {
        for (std::size_t i = 0; i < category.option_count; ++i) {
            const OptionDef& option = category.options[i];
            // the shader list is not known yet when the config is first applied
            if (option.type == OptionType::Choice && option.choices == kNoShaderChoices) {
                prefs_set_string(option.key, GetManager().GetOptionValue(option).c_str());
                continue;
            }
            StoreInPrefs(option);
        }
    }
}

const std::vector<OptionCategory>& GetCategories() {
    return kCategories;
}

const OptionDef* FindOption(std::string_view key) {
    for (const OptionCategory& category : kCategories) {
        for (std::size_t i = 0; i < category.option_count; ++i) {
            if (key == category.options[i].key) {
                return &category.options[i];
            }
        }
    }
    return nullptr;
}

void SetDynamicChoices(std::string_view key, std::vector<OptionChoice> choices) {
    for (OptionDef& option : kDisplayOptions) {
        if (key != option.key) {
            continue;
        }
        s_dynamic_choices = std::move(choices);
        if (s_dynamic_choices.empty()) {
            option.choices = kNoShaderChoices;
            option.choice_count = 1;
        } else {
            option.choices = s_dynamic_choices.data();
            option.choice_count = s_dynamic_choices.size();
        }
        return;
    }
}

std::string GetOptionValue(const OptionDef& option) {
    return GetManager().GetOptionValue(option);
}

bool IsOptionShown(const OptionDef& option) {
    if (!option.shown_when_key) {
        return true;
    }
    const OptionDef* controller = FindOption(option.shown_when_key);
    return controller && GetManager().GetOptionValue(*controller) == option.shown_when_value;
}

OptionValueLabel GetOptionValueLabel(const OptionDef& option) {
    const Manager& config = GetManager();
    switch (option.type) {
    case OptionType::Toggle:
        return config.GetBool(option) ? OptionValueLabel{"emulator_on", "On"}
                                      : OptionValueLabel{"emulator_off", "Off"};
    case OptionType::Choice: {
        const OptionChoice& choice = option.choices[config.GetChoiceIndex(option)];
        return {choice.label_key, choice.fallback};
    }
    case OptionType::Range:
        return {nullptr, std::to_string(config.GetInt(option))};
    case OptionType::Text:
    default:
        return {nullptr, config.GetOptionValue(option)};
    }
}

void SetOptionValue(const OptionDef& option, const std::string& value) {
    GetManager().SetConfigValue(option.key, value);
    GetManager().SaveConfig();
    StoreInPrefs(option);
}

void StepOption(const OptionDef& option, int direction) {
    if (direction == 0) {
        return;
    }
    const Manager& config = GetManager();
    switch (option.type) {
    case OptionType::Toggle:
        SetOptionValue(option, config.GetBool(option) ? "false" : "true");
        break;
    case OptionType::Choice: {
        const int count = static_cast<int>(option.choice_count);
        if (count == 0) {
            break;
        }
        const int index = (config.GetChoiceIndex(option) + (direction > 0 ? 1 : count - 1)) % count;
        SetOptionValue(option, option.choices[index].value);
        break;
    }
    case OptionType::Range: {
        const int value = std::clamp(
            config.GetInt(option) + (direction > 0 ? option.step : -option.step), option.min,
            option.max);
        SetOptionValue(option, std::to_string(value));
        break;
    }
    case OptionType::Text:
    default:
        break;
    }
}

std::string GetLoadedConfigPath() {
    return GetManager().GetLoadedPath();
}

std::size_t GetLoadedOptionCount() {
    return GetManager().GetOptionCount();
}

} // namespace SwitchFrontend::TicoConfig
