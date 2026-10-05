/* The tico quick menu for the Drastic host: the ImGui overlay drawn by the
 * active renderer, standing in for the host's own in-game menu. It takes the
 * same calls as DrasticIngameMenu, so main.c drives either one. */
#ifndef DRASTIC_TICO_MENU_H
#define DRASTIC_TICO_MENU_H

#include <switch.h>
#include <stdbool.h>

#include "drastic_config.h"
#include "ingame_menu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TicoMenu TicoMenu;

/* Reads sdmc:/tico/config/cores/drastic.jsonc. Call first: it names the
 * content folders (tico_nds_*_dir in config.h). */
void tico_config_load(void);

/* Copies the loaded settings into the preference store. Call after
 * prefs_init() and before the runtime config is read. */
void tico_menu_load_config(void);

/* Creates the overlay for the renderer chosen by drastic_renderer_select().
 * Call once the renderer has been initialized. */
TicoMenu *tico_menu_create(DrasticRuntimeConfig *config,
                           const DrasticMenuCore *core, int *state_slot);
void tico_menu_destroy(TicoMenu *menu);
/* Names the ROM file the core was given (a zip's unpacked copy), which names
 * the game's save and states, and backs up the cartridge save. */
void tico_menu_set_core_rom(TicoMenu *menu, const char *core_rom_path);
/* Offers to continue from the auto save; call once the game shows. */
void tico_menu_offer_resume(TicoMenu *menu);
/* Asks the core to save the game into the auto slot; call before the game
 * closes (exit, restart, HOME), then keep presenting frames until
 * tico_menu_auto_save_done, as the core finishes the save on its own thread
 * (it gives up after a few seconds). */
bool tico_menu_begin_auto_save(TicoMenu *menu);
bool tico_menu_auto_save_done(TicoMenu *menu);
/* True once after Restart was chosen (with the exit request). */
bool tico_menu_take_restart_request(TicoMenu *menu);
void tico_menu_open(TicoMenu *menu);
bool tico_menu_is_open(const TicoMenu *menu);
/* Feeds the pad to the open menu and runs what was chosen on the last frame. */
void tico_menu_update(TicoMenu *menu, u64 held, u64 pressed,
                      HidAnalogStickState left, HidAnalogStickState right);
bool tico_menu_take_exit_request(TicoMenu *menu);
/* The core keeps its own cheat toggles; this brings in the game's cheats from
 * tico's folder (sdmc:/tico/cheats/nds/) as custom cheats, off. Call once the
 * game runs, with the core paused. */
void tico_menu_apply_persisted_cheats(TicoMenu *menu);
/* The combo that opened the menu also closes it. */
void tico_menu_set_toggle_combo(TicoMenu *menu, u64 combo);

/* The layout hotkey chose another layout: keeps it as the setting and names
 * it in a toast. */
void tico_menu_layout_changed(const char *layout);

/* Started without a game: shows the game list until a game is chosen (this
 * NRO is then queued with it) or Exit, presenting a frame each time round. */
void tico_library_run(void (*present)(void *user), void *user);

/* Figures for the FPS counter drawn over the game. */
void tico_menu_set_hud(float fps, bool fast_forward, int rendered_width,
                       int rendered_height);

#ifdef __cplusplus
}
#endif

#endif
