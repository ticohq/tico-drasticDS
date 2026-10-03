// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "tico/overlay/tico_config.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
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

// ---------------------------------------------------------------------------
// Option catalogue, read from the module's settings.json packed in this NRO's
// romfs. tico reads its own copy from the installed module, so both list the
// same options. Option and tab labels are translation keys (lang/*.json);
// choice labels are English, translated through settings_drastic_value_<slug>
// keys when the language files have one (tools/tico_translations.py).

constexpr const char* kSettingsPath = "romfs:/module/settings.json";
constexpr const char* kSlug = "nds";

// Listed only in game: tico cannot see the shaders on the SD card.
constexpr const char* kShaderKey = "Wrapper/CustomShader";
constexpr const char* kFilterKey = "Wrapper/VideoFilter";
constexpr OptionChoice kNoShaderChoices[] = {
    {"", "settings_drastic_value_none_found", "None found"},
};

// The translation key of a choice label: settings_drastic_value_ and the label
// in lower case with runs of other characters turned into one underscore.
std::string ValueKey(std::string_view label) {
    std::string key = "settings_drastic_value_";
    bool separator = false;
    for (const char c : label) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            if (separator && key.back() != '_') {
                key.push_back('_');
            }
            separator = false;
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else {
            separator = true;
        }
    }
    return key;
}

struct Catalogue {
    // the catalogue's strings and arrays never move once built
    std::deque<std::string> strings;
    std::deque<std::vector<OptionChoice>> choice_lists;
    std::deque<std::vector<OptionDef>> option_lists;
    std::vector<OptionCategory> categories;

    const char* Keep(std::string text) {
        strings.push_back(std::move(text));
        return strings.back().c_str();
    }
};

OptionDef ShaderOption() {
    OptionDef option{kShaderKey, "settings_drastic_custom_shader", "Custom shader",
                     OptionType::Choice, "", kNoShaderChoices, 1, false, 0};
    option.shown_when_key = kFilterKey;
    option.shown_when_value = "custom";
    return option;
}

bool ParseOption(Catalogue& catalogue, const nlohmann::json& source, OptionDef& option) {
    const std::string type = source.value("type", "");
    if (!source.contains("key") || !source["key"].is_string()) {
        return false;
    }
    if (type == "bool") {
        option.type = OptionType::Toggle;
    } else if (type == "enum") {
        option.type = OptionType::Choice;
    } else if (type == "string") {
        option.type = OptionType::Text;
    } else {
        // actions and anything newer are tico's own
        return false;
    }
    option.key = catalogue.Keep(source["key"].get<std::string>());
    option.fallback = catalogue.Keep(source.value("label", source["key"].get<std::string>()));
    option.label_key = option.fallback;
    option.default_value =
        catalogue.Keep(source.contains("default") ? JsonScalarToString(source["default"]) : "");
    option.needs_restart = source.value("restart", false);
    option.max_length = source.value("max_length", 0);
    option.choices = nullptr;
    option.choice_count = 0;
    if (option.type == OptionType::Choice) {
        std::vector<OptionChoice> choices;
        for (const nlohmann::json& choice : source.value("choices", nlohmann::json::array())) {
            const std::string value =
                choice.contains("value") ? JsonScalarToString(choice["value"]) : "";
            const std::string label = choice.value("label", value);
            choices.push_back({catalogue.Keep(value), catalogue.Keep(ValueKey(label)),
                               catalogue.Keep(label)});
        }
        if (choices.empty()) {
            return false;
        }
        catalogue.choice_lists.push_back(std::move(choices));
        option.choices = catalogue.choice_lists.back().data();
        option.choice_count = catalogue.choice_lists.back().size();
    }
    if (source.contains("depends_on") && source["depends_on"].is_object()) {
        const nlohmann::json& depends_on = source["depends_on"];
        option.shown_when_key = catalogue.Keep(depends_on.value("key", ""));
        option.shown_when_value = catalogue.Keep(
            depends_on.contains("value") ? JsonScalarToString(depends_on["value"]) : "");
    }
    return true;
}

Catalogue LoadCatalogue() {
    Catalogue catalogue;
    std::string content;
    nlohmann::json root;
    if (ReadWholeFile(kSettingsPath, content)) {
        root = nlohmann::json::parse(StripJsonComments(content), nullptr, false);
    }
    if (!root.is_object()) {
        tico_log("could not read the settings definition at %s\n", kSettingsPath);
    }

    for (const nlohmann::json& tab : root.value("tabs", nlohmann::json::array())) {
        std::vector<OptionDef> options;
        for (const nlohmann::json& section : tab.value("sections", nlohmann::json::array())) {
            for (const nlohmann::json& source : section.value("options", nlohmann::json::array())) {
                OptionDef option{};
                if (!ParseOption(catalogue, source, option)) {
                    continue;
                }
                options.push_back(option);
                if (std::strcmp(option.key, kFilterKey) == 0) {
                    options.push_back(ShaderOption());
                }
            }
        }
        if (options.empty()) {
            continue;
        }
        catalogue.option_lists.push_back(std::move(options));
        const std::vector<OptionDef>& stored = catalogue.option_lists.back();
        const char* name = catalogue.Keep(tab.value("name", "Settings"));
        catalogue.categories.push_back({name, name, stored.data(), stored.size()});
    }
    // the menu always has a category to show, even without a definition
    if (catalogue.categories.empty()) {
        catalogue.categories.push_back({nullptr, "Settings", nullptr, 0});
    }
    return catalogue;
}

Catalogue& GetCatalogue() {
    static Catalogue catalogue = LoadCatalogue();
    return catalogue;
}

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
        original = nlohmann::json::object();
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
            original = std::move(root);
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
        changed[key] = value;
    }

    // Writes back what was read, with this session's changes as strings, the
    // way tico stores them (bools as settings.json's bool_true_value and
    // bool_false_value). Keys tico wrote keep their own JSON types.
    bool SaveConfig() {
        nlohmann::json root = original.is_object() ? original : nlohmann::json::object();
        for (const auto& [key, value] : changed) {
            root[key] = value;
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
    OptionMap changed;
    nlohmann::json original = nlohmann::json::object();
    std::string loaded_path;
};

Manager& GetManager() {
    static Manager manager;
    return manager;
}

std::vector<OptionChoice> s_dynamic_choices;

// The value written to prefs: toggles as true/false, and a choice that the file
// names but the list does not know replaced by a known one.
std::string PrefsValue(const OptionDef& option) {
    const Manager& config = GetManager();
    switch (option.type) {
    case OptionType::Toggle:
        return config.GetBool(option) ? "true" : "false";
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

std::string ContentPath(const char* key, const char* default_root) {
    std::string root = GetManager().GetConfigValue(key, "");
    if (root.empty()) {
        root = default_root;
    }
    while (root.size() > 1 && root.back() == '/') {
        root.pop_back();
    }
    return root + "/" + kSlug;
}

} // namespace

void ReloadConfig() {
    GetManager().ReloadConfig();
}

std::string SystemPath() {
    return ContentPath("tico_system_path", "sdmc:/tico/system");
}

std::string SavesPath() {
    return ContentPath("tico_saves_path", "sdmc:/tico/saves");
}

std::string StatesPath() {
    return ContentPath("tico_states_path", "sdmc:/tico/states");
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
    for (const OptionCategory& category : GetCategories()) {
        for (std::size_t i = 0; i < category.option_count; ++i) {
            const OptionDef& option = category.options[i];
            // the shader list is not known yet when the config is first applied
            if (std::strcmp(option.key, kShaderKey) == 0) {
                prefs_set_string(option.key, GetManager().GetOptionValue(option).c_str());
                continue;
            }
            StoreInPrefs(option);
        }
    }
}

const std::vector<OptionCategory>& GetCategories() {
    return GetCatalogue().categories;
}

const OptionDef* FindOption(std::string_view key) {
    for (const OptionCategory& category : GetCategories()) {
        for (std::size_t i = 0; i < category.option_count; ++i) {
            if (key == category.options[i].key) {
                return &category.options[i];
            }
        }
    }
    return nullptr;
}

void SetDynamicChoices(std::string_view key, std::vector<OptionChoice> choices) {
    for (std::vector<OptionDef>& options : GetCatalogue().option_lists) {
        for (OptionDef& option : options) {
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
}

std::string GetOptionValue(const OptionDef& option) {
    return GetManager().GetOptionValue(option);
}

bool IsOptionShown(const OptionDef& option) {
    if (!option.shown_when_key) {
        return true;
    }
    const OptionDef* controller = FindOption(option.shown_when_key);
    return controller && PrefsValue(*controller) == option.shown_when_value;
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
