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

/* Loads sdmc:/tico/config/cores/drastic.jsonc into the preference store.
 * Call after prefs_init() and before the runtime config is read. */
void tico_menu_load_config(void);

/* Creates the overlay for the renderer chosen by drastic_renderer_select().
 * Call once the renderer has been initialized. */
TicoMenu *tico_menu_create(DrasticRuntimeConfig *config,
                           const DrasticMenuCore *core, int *state_slot);
void tico_menu_destroy(TicoMenu *menu);
void tico_menu_open(TicoMenu *menu);
bool tico_menu_is_open(const TicoMenu *menu);
/* Feeds the pad to the open menu and runs what was chosen on the last frame. */
void tico_menu_update(TicoMenu *menu, u64 held, u64 pressed,
                      HidAnalogStickState left, HidAnalogStickState right);
bool tico_menu_take_exit_request(TicoMenu *menu);
/* The core keeps cheat toggles in usrcheat.dat itself, so this does nothing;
 * it exists so main.c treats both menus alike. */
void tico_menu_apply_persisted_cheats(TicoMenu *menu);
/* The combo that opened the menu also closes it. */
void tico_menu_set_toggle_combo(TicoMenu *menu, u64 combo);

/* Figures for the FPS counter drawn over the game. */
void tico_menu_set_hud(float fps, bool fast_forward);

#ifdef __cplusplus
}
#endif

#endif
