// The game list shown when the host starts without a game: the Nintendo DS
// games in tico's ROM bases (<base>/nds/) and the module's own folders
// (tico_rom_folders in drastic.jsonc, edited from Settings > Library).

#include "tico/tico_library.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

#include <json.hpp>

#include "tico/UsbStorage.h"
#include "tico/overlay/overlay_ui.h"
#include "tico/overlay/tico_config.h"

namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace TicoConfig = SwitchFrontend::TicoConfig;

namespace TicoLibrary {
namespace {

constexpr const char* kSlug = "nds";
constexpr const char* kExtensions[] = {".nds", ".zip", ".7z", ".rar"};

std::function<void(const std::string&)> s_launch;

std::string LowerExtension(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string extension = name.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

std::string WithSlash(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.empty() && path.back() != '/')
        path += '/';
    return path;
}

// The file name without its extension, (tags) or [tags].
std::string CleanTitle(const std::string& filename) {
    std::string title = filename.substr(0, filename.find_last_of('.'));
    std::string result;
    int depth = 0;
    for (const char c : title) {
        if (c == '(' || c == '[') {
            depth++;
        } else if (c == ')' || c == ']') {
            if (depth > 0)
                depth--;
        } else if (!depth && !(c == ' ' && (result.empty() || result.back() == ' '))) {
            result += c;
        }
    }
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result.empty() ? filename : result;
}

nlohmann::json ReadJsonFile(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file)
        return {};
    std::string text;
    char buffer[4096];
    std::size_t count;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        text.append(buffer, count);
    std::fclose(file);
    return nlohmann::json::parse(text, nullptr, false, true);
}

// tico's ROM bases (general.jsonc): the ROMs path, then the extra bases.
std::vector<std::string> TicoRomBases() {
    std::vector<std::string> bases;
    const nlohmann::json general = ReadJsonFile("sdmc:/tico/config/general.jsonc");
    const std::string roms =
        general.is_object() ? general.value("roms_path", std::string()) : std::string();
    bases.push_back(WithSlash(roms.empty() ? "sdmc:/tico/roms/" : roms));
    if (general.is_object() && general.contains("rom_base_paths") &&
        general["rom_base_paths"].is_array())
        for (const auto& base : general["rom_base_paths"])
            if (base.is_string() && !base.get<std::string>().empty())
                bases.push_back(WithSlash(base.get<std::string>()));
    return bases;
}

nlohmann::json AllModuleFolders() {
    const std::string text = TicoConfig::GetConfigJson("tico_rom_folders");
    nlohmann::json folders =
        text.empty() ? nlohmann::json::object() : nlohmann::json::parse(text, nullptr, false);
    return folders.is_object() ? folders : nlohmann::json::object();
}

std::vector<std::string> ModuleFolders() {
    std::vector<std::string> folders;
    const nlohmann::json all = AllModuleFolders();
    const auto it = all.find(kSlug);
    if (it != all.end() && it->is_array())
        for (const auto& entry : *it)
            if (entry.is_string() && !entry.get<std::string>().empty())
                folders.push_back(WithSlash(entry.get<std::string>()));
    return folders;
}

void SetModuleFolders(const std::vector<std::string>& folders) {
    nlohmann::json all = AllModuleFolders();
    if (folders.empty())
        all.erase(kSlug);
    else
        all[kSlug] = folders;
    TicoConfig::SetConfigJson("tico_rom_folders", all.dump());
    TicoConfig::SaveConfig();
}

void ScanFolder(const std::string& dir, int depth, std::vector<std::string>& out) {
    DIR* directory = opendir(dir.c_str());
    if (!directory)
        return;
    while (struct dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        const std::string path = WithSlash(dir) + name;
        bool is_dir = entry->d_type == DT_DIR;
        if (entry->d_type == DT_UNKNOWN) {
            struct stat status;
            is_dir = stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
        }
        if (is_dir) {
            if (depth > 0)
                ScanFolder(path, depth - 1, out);
            continue;
        }
        const std::string extension = LowerExtension(name);
        for (const char* known : kExtensions)
            if (extension == known)
                out.push_back(path);
    }
    closedir(directory);
}

std::vector<OverlayUI::LibraryEntry> List() {
    std::vector<std::string> folders;
    for (const std::string& base : TicoRomBases())
        folders.push_back(base + kSlug + "/");
    for (const std::string& folder : ModuleFolders())
        folders.push_back(folder);
    std::vector<std::string> games;
    for (const std::string& folder : folders) {
        // a USB folder only while its drive is connected
        const std::string mounted = UsbStorage::Resolve(folder);
        if (!mounted.empty())
            ScanFolder(mounted, 2, games);
    }
    std::sort(games.begin(), games.end());
    games.erase(std::unique(games.begin(), games.end()), games.end());
    std::vector<OverlayUI::LibraryEntry> entries;
    for (const std::string& path : games)
        entries.push_back({CleanTitle(path.substr(path.find_last_of('/') + 1)), "NDS", path});
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
    });
    return entries;
}

} // namespace

void Register(std::function<void(const std::string& path)> launch) {
    s_launch = std::move(launch);

    OverlayUI::LibraryCallbacks library;
    library.list = [] { return List(); };
    library.launch = [](const std::string& path) {
        // a game on a USB drive goes by its drive id, as tico passes it
        if (s_launch)
            s_launch(UsbStorage::ToToken(path));
    };
    OverlayUI::SetLibraryCallbacks(std::move(library));

    OverlayUI::LibraryFolderCallbacks folders;
    folders.groups = [] {
        OverlayUI::LibraryFolderGroup group;
        group.label = "Nintendo DS";
        for (const std::string& base : TicoRomBases())
            group.bases.push_back(base + kSlug + "/");
        group.folders = ModuleFolders();
        return std::vector<OverlayUI::LibraryFolderGroup>{group};
    };
    folders.set = [](int group, const std::vector<std::string>& paths) {
        if (group != 0)
            return;
        std::vector<std::string> normalized;
        for (const std::string& path : paths)
            normalized.push_back(WithSlash(path));
        SetModuleFolders(normalized);
    };
    OverlayUI::SetLibraryFolderCallbacks(std::move(folders));
}

void Unregister() {
    OverlayUI::SetLibraryCallbacks({});
    OverlayUI::SetLibraryFolderCallbacks({});
    s_launch = nullptr;
}

} // namespace TicoLibrary
