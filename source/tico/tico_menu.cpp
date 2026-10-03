// The tico quick menu for the Drastic host. The overlay draws inside the
// renderer's present call on the main loop thread, and every action it
// returns is carried out here on the next update, with the core paused.

#include <switch.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "config.h"
#include "drastic_config.h"
#include "drastic_custom_shader.h"
#include "drastic_renderer.h"
#include "ingame_menu.h"
#include "jni_fake.h"
#include "opensles.h"
#include "overlay.h"
#include "prefs.h"
#include "tico/tico_entry.h"
#include "tico/tico_menu.h"
}

#include "tico/overlay/imgui_overlay.h"
#include "tico/overlay/overlay_ui.h"
#include "tico/overlay/tico_config.h"

namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace ImGuiOverlay = SwitchFrontend::ImGuiOverlay;
namespace TicoConfig = SwitchFrontend::TicoConfig;

namespace {

constexpr int kSnapshotPixels = 256 * 192;
// the overlay's save-state slots 1..4 are Drastic's slots 0..3
constexpr int kOverlaySlotCount = 4;
// the cheat list's first row adds a custom cheat
constexpr int kAddCheatRow = -1;
constexpr u64 kRepeatDelayMs = 400;
constexpr u64 kRepeatRateMs = 80;

// Packed-config fields that are only applied when a game starts (see
// drastic_config_build_core_config): the ones the host's own menu never
// changes in game. They keep their launch value while the game runs;
// high-resolution 3D in particular changes the size of the frames the
// renderer was built for.
constexpr uint64_t kLaunchOnlyConfigBits =
    (UINT64_C(3) << 8) |   // audio latency
    (UINT64_C(3) << 16) |  // CPU worker threads
    (UINT64_C(1) << 23) |  // 16-bit color
    (UINT64_C(1) << 24) |  // ignore game card size limit
    (UINT64_C(1) << 25) |  // backup data in save states
    (UINT64_C(1) << 35) |  // fix main-engine screen
    (UINT64_C(1) << 36) |  // auto-trim
    (UINT64_C(1) << 39) |  // RTC from system time
    (UINT64_C(1) << 40) |  // disable edge marking
    (UINT64_C(1) << 41) |  // high-resolution 3D
    (UINT64_C(1) << 42) |  // Lua scripts
    (UINT64_C(7) << 43) |  // Slot-2 accessory
    (UINT64_C(1) << 48) |  // preload ROM
    (UINT64_C(1) << 49) |  // frame blending
    (UINT64_C(1) << 50);   // raw save format

struct Cheat {
    std::string name;
    int index = 0;
    int folder = -1;
    bool custom = false;
    bool enabled = false;
};

std::string JavaBytes(void* array) {
    std::string text;
    const uint8_t* data = jni_byte_array_data(array);
    int length = jni_byte_array_length(array);
    if (data && length > 0) {
        while (length > 0 && !data[length - 1])
            length--;
        text.assign(reinterpret_cast<const char*>(data), static_cast<std::size_t>(length));
    }
    jni_release_byte_array(array);
    return text;
}

bool PromptKeyboard(const char* header, const char* guide, const std::string& initial,
                    std::size_t max_length, bool multiline, std::string& output) {
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0)))
        return false;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, header);
    if (guide)
        swkbdConfigSetGuideText(&keyboard, guide);
    if (!initial.empty())
        swkbdConfigSetInitialText(&keyboard, initial.c_str());
    swkbdConfigSetStringLenMax(&keyboard, static_cast<u32>(max_length));
    swkbdConfigSetReturnButtonFlag(&keyboard, multiline ? 1 : 0);
    std::vector<char> text(max_length + 1);
    const Result result = swkbdShow(&keyboard, text.data(), text.size());
    swkbdClose(&keyboard);
    if (R_FAILED(result))
        return false;
    output = text.data();
    return true;
}

// Action Replay text as address/value words, or an empty list if it is not
// whole 8-digit pairs.
std::vector<int32_t> ParseCheatWords(const std::string& text) {
    std::vector<int32_t> words;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) ||
                                   text[i] == ',' || text[i] == ';' || text[i] == ':'))
            i++;
        if (i >= text.size())
            break;
        if (text[i] == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X'))
            i += 2;
        uint32_t value = 0;
        int digits = 0;
        while (i < text.size() && std::isxdigit(static_cast<unsigned char>(text[i])) && digits < 8) {
            const char c = text[i++];
            value = (value << 4) |
                    static_cast<uint32_t>(c <= '9' ? c - '0' : (std::toupper(c) - 'A' + 10));
            digits++;
        }
        if (!digits || (i < text.size() && std::isxdigit(static_cast<unsigned char>(text[i]))))
            return {};
        words.push_back(static_cast<int32_t>(value));
    }
    if (words.size() < 2 || (words.size() & 1))
        return {};
    return words;
}

} // namespace

struct TicoMenu {
    DrasticRuntimeConfig* config = nullptr;
    DrasticMenuCore core{};
    int* state_slot = nullptr;
    bool open = false;
    bool exit_requested = false;
    u64 toggle_combo = 0;

    // held d-pad direction and when it next repeats
    u64 repeat_button = 0;
    u64 repeat_at = 0;

    void* snapshot_top_array = nullptr;
    void* snapshot_bottom_array = nullptr;

    std::vector<Cheat> cheats;
    std::vector<int> folder_single_select;

    std::vector<DrasticCustomShaderEntry> shaders;
    std::vector<TicoConfig::OptionChoice> shader_choices;

    void Call(void (*function)(void*, void*, int), int value) {
        if (function)
            function(core.env, core.clazz, value);
    }

    void Pause(bool paused) {
        Call(core.pause_system, paused ? 1 : 0);
    }

    void Close(bool resume) {
        if (!open)
            return;
        open = false;
        ImGuiOverlay::SetVisible(false);
        if (resume)
            Pause(false);
    }

    // ---------------------------------------------------------------------
    // Save states

    bool SlotOccupied(int slot) {
        if (!core.get_snapshots || !snapshot_top_array || !snapshot_bottom_array)
            return false;
        int32_t* top = jni_int_array_data(snapshot_top_array);
        int32_t* bottom = jni_int_array_data(snapshot_bottom_array);
        if (!top || !bottom)
            return false;
        std::memset(top, 0, kSnapshotPixels * sizeof(*top));
        std::memset(bottom, 0, kSnapshotPixels * sizeof(*bottom));
        core.get_snapshots(core.env, core.clazz, slot, snapshot_top_array, snapshot_bottom_array);
        for (int i = 0; i < kSnapshotPixels; i++) {
            if (top[i] || bottom[i])
                return true;
        }
        return false;
    }

    void SaveState(int slot) {
        const bool requested =
            core.save_state && core.save_state(core.env, core.clazz, slot, 1);
        char message[64];
        std::snprintf(message, sizeof(message),
                      requested ? "Saving state to slot %d" : "Slot %d could not be saved",
                      slot + 1);
        OverlayUI::ShowToast(message);
    }

    void LoadState(int slot) {
        const bool loaded = core.load_state && core.load_state(core.env, core.clazz, slot);
        char message[64];
        std::snprintf(message, sizeof(message),
                      loaded ? "Loaded state from slot %d" : "Slot %d could not be loaded",
                      slot + 1);
        OverlayUI::ShowToast(message);
    }

    // ---------------------------------------------------------------------
    // Cheats (database cheats from usrcheat.dat, then the title's custom ones)

    void RefreshCheats() {
        cheats.clear();
        folder_single_select.clear();
        const int folder_count =
            core.get_cheat_folder_count ? core.get_cheat_folder_count(core.env, core.clazz) : 0;
        for (int folder = 0; folder < folder_count; folder++) {
            if (core.get_cheat_folder_multi_select &&
                !core.get_cheat_folder_multi_select(core.env, core.clazz, folder))
                folder_single_select.push_back(folder);
        }
        const int database_count =
            core.get_cheat_count ? core.get_cheat_count(core.env, core.clazz) : 0;
        for (int i = 0; i < database_count; i++) {
            Cheat cheat;
            cheat.index = i;
            cheat.folder = core.get_cheat_folder_id
                               ? core.get_cheat_folder_id(core.env, core.clazz, i)
                               : -1;
            cheat.enabled =
                core.get_cheat_enabled && core.get_cheat_enabled(core.env, core.clazz, i);
            if (core.get_cheat_name)
                cheat.name = JavaBytes(core.get_cheat_name(core.env, core.clazz, i));
            if (cheat.name.empty())
                cheat.name = "Cheat " + std::to_string(i + 1);
            cheats.push_back(std::move(cheat));
        }
        const int custom_count =
            core.get_custom_cheat_count ? core.get_custom_cheat_count(core.env, core.clazz) : 0;
        for (int i = 0; i < custom_count; i++) {
            Cheat cheat;
            cheat.index = i;
            cheat.custom = true;
            cheat.enabled = core.get_custom_cheat_enabled &&
                            core.get_custom_cheat_enabled(core.env, core.clazz, i);
            if (core.get_custom_cheat_name)
                cheat.name = JavaBytes(core.get_custom_cheat_name(core.env, core.clazz, i));
            if (cheat.name.empty())
                cheat.name = "Custom cheat " + std::to_string(i + 1);
            cheats.push_back(std::move(cheat));
        }
    }

    std::vector<OverlayUI::CheatMenuEntry> CheatEntries() {
        RefreshCheats();
        std::vector<OverlayUI::CheatMenuEntry> entries;
        OverlayUI::CheatMenuEntry add;
        add.name = "+ Add custom Action Replay cheat";
        add.source_index = kAddCheatRow;
        add.is_add_row = true;
        entries.push_back(add);
        for (std::size_t i = 0; i < cheats.size(); i++) {
            OverlayUI::CheatMenuEntry entry;
            entry.name = cheats[i].custom ? cheats[i].name + " (custom)" : cheats[i].name;
            entry.enabled = cheats[i].enabled;
            entry.source_index = static_cast<int>(i);
            entries.push_back(entry);
        }
        return entries;
    }

    bool InSingleSelectFolder(const Cheat& cheat) const {
        for (const int folder : folder_single_select) {
            if (folder == cheat.folder)
                return true;
        }
        return false;
    }

    bool ToggleCheat(int source_index) {
        if (source_index < 0 || source_index >= static_cast<int>(cheats.size()))
            return false;
        Cheat& cheat = cheats[static_cast<std::size_t>(source_index)];
        cheat.enabled = !cheat.enabled;
        // a folder that allows one choice turns its other cheats off
        if (!cheat.custom && cheat.enabled && cheat.folder >= 0 && InSingleSelectFolder(cheat)) {
            for (Cheat& other : cheats) {
                if (&other == &cheat || other.custom || other.folder != cheat.folder ||
                    !other.enabled)
                    continue;
                other.enabled = false;
                if (core.set_cheat_enabled)
                    core.set_cheat_enabled(core.env, core.clazz, other.index, 0);
            }
        }
        if (cheat.custom && core.set_custom_cheat_enabled)
            core.set_custom_cheat_enabled(core.env, core.clazz, cheat.index, cheat.enabled);
        else if (!cheat.custom && core.set_cheat_enabled)
            core.set_cheat_enabled(core.env, core.clazz, cheat.index, cheat.enabled);
        if (core.update_cheats)
            core.update_cheats(core.env, core.clazz, 1);
        return true;
    }

    void AddCustomCheat() {
        if (!core.add_custom_cheat) {
            OverlayUI::ShowToast("Custom cheats are unavailable in this core");
            return;
        }
        std::string name;
        std::string codes;
        if (!PromptKeyboard("New custom cheat", "Enter a name", "", 95, false, name) ||
            name.empty())
            return;
        if (!PromptKeyboard("Action Replay code", "Enter hexadecimal address/value pairs", "",
                            4095, true, codes))
            return;
        const std::vector<int32_t> words = ParseCheatWords(codes);
        if (words.empty()) {
            OverlayUI::ShowToast("Invalid code: use 8-digit address/value pairs");
            return;
        }
        void* array = jni_make_int_array(static_cast<int>(words.size()));
        int32_t* data = jni_int_array_data(array);
        if (!data) {
            OverlayUI::ShowToast("Could not allocate the cheat code");
            return;
        }
        std::memcpy(data, words.data(), words.size() * sizeof(*data));
        void* java_name = jni_make_string(name.c_str());
        const int added = core.add_custom_cheat(core.env, core.clazz, java_name, array,
                                                static_cast<int>(words.size()), 1);
        jni_release_string(java_name);
        jni_release_int_array(array);
        if (core.update_cheats)
            core.update_cheats(core.env, core.clazz, 1);
        OverlayUI::RefreshCheatList();
        OverlayUI::ShowToast(added ? "Custom cheat added" : "Drastic rejected the custom cheat");
    }

    // ---------------------------------------------------------------------
    // Settings

    void RefreshShaders() {
        shaders.resize(128);
        shaders.resize(drastic_custom_shader_scan(shaders.data(), shaders.size()));
        shader_choices.clear();
        for (const DrasticCustomShaderEntry& shader : shaders)
            shader_choices.push_back({shader.relative_path, nullptr, shader.name});
        TicoConfig::SetDynamicChoices("Wrapper/CustomShader", shader_choices);
    }

    // Brings the running game in line with the settings just changed in the
    // menu. Settings only read at launch are left for the next start.
    void ApplySettings() {
        DrasticRuntimeConfig fresh;
        drastic_config_load(&fresh);
        DrasticRuntimeConfig& live = *config;

        if (fresh.video_filter == DRASTIC_FILTER_CUSTOM &&
            (live.video_filter != DRASTIC_FILTER_CUSTOM ||
             std::strcmp(fresh.custom_shader, live.custom_shader) != 0)) {
            char error[192] = "";
            if (!drastic_renderer_set_custom_shader(fresh.custom_shader, error, sizeof(error))) {
                // keep showing the game: go back to the filter that worked
                const TicoConfig::OptionDef* filter = TicoConfig::FindOption("Wrapper/VideoFilter");
                const TicoConfig::OptionDef* shader = TicoConfig::FindOption("Wrapper/CustomShader");
                if (filter)
                    TicoConfig::SetOptionValue(*filter, drastic_config_filter_name(live.video_filter));
                if (shader)
                    TicoConfig::SetOptionValue(*shader, live.custom_shader);
                OverlayUI::ShowToast(error[0] ? error : "This custom shader could not be loaded");
                return;
            }
        }

        live.layout = fresh.layout;
        live.swap_screens = fresh.swap_screens;
        live.screen_gap = fresh.screen_gap;
        live.integer_scale = fresh.integer_scale;
        live.custom_aspect_lock = fresh.custom_aspect_lock;
        std::memcpy(live.custom_screens, fresh.custom_screens, sizeof(live.custom_screens));
        if (live.rotation != fresh.rotation) {
            live.rotation = fresh.rotation;
            overlay_set_rotation(live.rotation);
        }
        live.video_filter = fresh.video_filter;
        live.fsr_sharpness = fresh.fsr_sharpness;
        std::snprintf(live.custom_shader, sizeof(live.custom_shader), "%s", fresh.custom_shader);
        drastic_config_calculate_layout(&live, panel_width, panel_height);

        if (live.volume != fresh.volume) {
            live.volume = fresh.volume;
            Call(core.set_audio_volume, live.volume);
            opensles_set_master_volume(static_cast<unsigned>(live.volume));
        }
        if (live.microphone_enabled != fresh.microphone_enabled) {
            live.microphone_enabled = fresh.microphone_enabled;
            opensles_set_microphone_enabled(live.microphone_enabled != 0);
        }
        if (live.microphone_source != fresh.microphone_source) {
            live.microphone_source = fresh.microphone_source;
            opensles_set_microphone_source(live.microphone_source == DRASTIC_MICROPHONE_EXTERNAL
                                               ? OPENSLES_MIC_SOURCE_EXTERNAL
                                               : OPENSLES_MIC_SOURCE_SIMULATED);
        }
        if (live.autosave_seconds != fresh.autosave_seconds) {
            live.autosave_seconds = fresh.autosave_seconds;
            Call(core.set_autosave_interval, live.autosave_seconds);
        }
        live.vibration = fresh.vibration;
        live.motion = fresh.motion;
        live.stylus_mode = fresh.stylus_mode;
        live.mouse_stylus = fresh.mouse_stylus;
        live.motion_stylus_sensitivity = fresh.motion_stylus_sensitivity;

        const uint64_t core_config = (fresh.core_config & ~kLaunchOnlyConfigBits) |
                                     (live.core_config & kLaunchOnlyConfigBits);
        if (live.core_config != core_config) {
            const bool cheats_changed = ((live.core_config ^ core_config) >> 27) & 1;
            live.core_config = core_config;
            if (core.apply_config)
                core.apply_config(core.env, core.clazz, static_cast<DrasticJLong>(live.core_config));
            if (cheats_changed && core.update_cheats)
                core.update_cheats(core.env, core.clazz, 1);
        }

        // frame generation can be switched for this session when the
        // renderer prepared it at launch
        const bool lsfg = prefs_get_bool("Wrapper/LSFGEnabled", false);
        if (lsfg != drastic_renderer_lsfg_enabled()) {
            if (!drastic_renderer_lsfg_available() ||
                !drastic_renderer_lsfg_request_enabled(lsfg))
                OverlayUI::ShowToast("Frame generation applies the next time the game starts",
                                     OverlayUI::ToastCorner::TopRight);
        }
    }

    void EditText() {
        const TicoConfig::OptionDef* option = OverlayUI::ConsumeTextEditOption();
        if (!option)
            return;
        std::string value;
        const std::size_t length = option->max_length > 0 ? option->max_length : 64;
        if (!PromptKeyboard(option->fallback, nullptr, TicoConfig::GetOptionValue(*option),
                            length, false, value))
            return;
        TicoConfig::SetOptionValue(*option, value);
        OverlayUI::NotifyOptionEdited(*option);
    }

    // ---------------------------------------------------------------------

    void RunAction(OverlayUI::Action action) {
        using OverlayUI::Action;
        switch (action) {
        case Action::None:
            return;
        case Action::Resume:
            Close(true);
            return;
        case Action::Exit:
            exit_requested = true;
            // like Quit in the host's menu, the core stays paused until shutdown
            Close(false);
            return;
        case Action::Reset:
            if (core.reset_ds)
                core.reset_ds(core.env, core.clazz);
            Close(true);
            return;
        case Action::EditText:
            EditText();
            return;
        case Action::AddCheat:
            AddCustomCheat();
            return;
        default:
            break;
        }
        if (OverlayUI::IsSaveStateAction(action)) {
            SaveState(OverlayUI::GetStateSlotForAction(action) - 1);
            Close(true);
        } else if (OverlayUI::IsLoadStateAction(action)) {
            LoadState(OverlayUI::GetStateSlotForAction(action) - 1);
            Close(true);
        }
    }

    // Edge-triggered directions, repeating while one is held.
    u64 Repeated(u64 held, u64 pressed) {
        constexpr u64 kDirections = HidNpadButton_AnyUp | HidNpadButton_AnyDown |
                                    HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
        const u64 now = armTicksToNs(armGetSystemTick()) / 1000000;
        if (pressed & kDirections) {
            repeat_button = pressed & kDirections;
            repeat_at = now + kRepeatDelayMs;
            return pressed;
        }
        if (!repeat_button || !(held & repeat_button)) {
            repeat_button = 0;
            return pressed;
        }
        if (now >= repeat_at) {
            repeat_at = now + kRepeatRateMs;
            return pressed | repeat_button;
        }
        return pressed;
    }
};

extern "C" {

void tico_menu_load_config(void) {
    TicoConfig::ReloadConfig();
    TicoConfig::ApplyToPrefs();
    OverlayUI::ReloadSettings();
}

TicoMenu* tico_menu_create(DrasticRuntimeConfig* config, const DrasticMenuCore* core,
                           int* state_slot) {
    if (!config || !core || !state_slot)
        return nullptr;
    TicoMenu* menu = new TicoMenu();
    menu->config = config;
    menu->core = *core;
    menu->state_slot = state_slot;
    menu->snapshot_top_array = jni_make_int_array(kSnapshotPixels);
    menu->snapshot_bottom_array = jni_make_int_array(kSnapshotPixels);
    menu->RefreshShaders();

    OverlayUI::SetGameTitle(tico_display_title());
    OverlayUI::SetSlotOccupiedCallback(
        [menu](int slot) { return slot >= 1 && slot <= kOverlaySlotCount && menu->SlotOccupied(slot - 1); });
    OverlayUI::SetCheatCallbacks([menu] { return menu->CheatEntries(); },
                                 [menu](int index) { return menu->ToggleCheat(index); });
    ImGuiOverlay::Init(drastic_renderer_is_vulkan());
    return menu;
}

void tico_menu_destroy(TicoMenu* menu) {
    if (!menu)
        return;
    ImGuiOverlay::Shutdown();
    OverlayUI::SetSlotOccupiedCallback(nullptr);
    OverlayUI::SetCheatCallbacks(nullptr, nullptr);
    jni_release_int_array(menu->snapshot_top_array);
    jni_release_int_array(menu->snapshot_bottom_array);
    delete menu;
}

void tico_menu_open(TicoMenu* menu) {
    if (!menu || menu->open)
        return;
    menu->open = true;
    menu->repeat_button = 0;
    menu->Pause(true);
    ImGuiOverlay::SetVisible(true);
}

bool tico_menu_is_open(const TicoMenu* menu) {
    return menu && menu->open;
}

void tico_menu_update(TicoMenu* menu, u64 held, u64 pressed, HidAnalogStickState left,
                      HidAnalogStickState right) {
    (void)left;
    (void)right;
    if (!menu || !menu->open)
        return;

    menu->RunAction(ImGuiOverlay::ConsumeAction());
    if (OverlayUI::ConsumeSettingsChanged())
        menu->ApplySettings();
    if (!menu->open)
        return;

    if (menu->toggle_combo && (held & menu->toggle_combo) == menu->toggle_combo &&
        (pressed & menu->toggle_combo)) {
        menu->Close(true);
        return;
    }

    pressed = menu->Repeated(held, pressed);
    ImGuiOverlay::FeedNav({
        .up = (pressed & HidNpadButton_AnyUp) != 0,
        .down = (pressed & HidNpadButton_AnyDown) != 0,
        .left = (pressed & HidNpadButton_AnyLeft) != 0,
        .right = (pressed & HidNpadButton_AnyRight) != 0,
        .accept = (pressed & HidNpadButton_A) != 0,
        .cancel = (pressed & HidNpadButton_B) != 0,
    });
}

bool tico_menu_take_exit_request(TicoMenu* menu) {
    if (!menu || !menu->exit_requested)
        return false;
    menu->exit_requested = false;
    return true;
}

void tico_menu_apply_persisted_cheats(TicoMenu* menu) {
    (void)menu;
}

void tico_menu_set_toggle_combo(TicoMenu* menu, u64 combo) {
    if (menu)
        menu->toggle_combo = combo;
}

void tico_menu_set_hud(float fps, bool fast_forward) {
    OverlayUI::HudStats stats;
    stats.fps = fps;
    stats.fast_forward = fast_forward;
    OverlayUI::SetHudStats(stats);
}

} // extern "C"
