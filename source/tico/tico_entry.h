/* tico frontend entry for the Drastic host.
 *
 * tico chainloads tico-drastic.nro with the ROM path in argv[1] and the game's
 * display title in argv[2]; on exit the host chainloads back to tico with
 * --resume. Started without a ROM, it shows a list of the games in tico's ROM
 * folders and launches the chosen one. */
#ifndef DRASTIC_TICO_ENTRY_H
#define DRASTIC_TICO_ENTRY_H

#include <stdbool.h>
#include <stddef.h>

#define TICO_LAUNCHER_PATH "sdmc:/switch/tico/tico.nro"

/* Creates tico's folders; the host's own folders are created inside them.
 * Call once the tico config is loaded, since it names the content folders. */
void tico_make_directories(void);

/* Creates a directory and any missing parents. */
void tico_make_path(const char *path);

/* Installs the bundled game database and stages the BIOS/firmware from tico's
 * NDS system folder. Call after prefs_init(); it points Drastic/RomPath and
 * Wrapper/CoreSo at the right files for this launch. */
void tico_prepare(int argc, char **argv);

const char *tico_display_title(void);

/* The game as tico named it (the archive, or usb://...), which names its
 * per-game settings. */
const char *tico_rom_path(void);

/* True when this launch is a Restart (tico_queue_restart): the game starts
 * over instead of offering to continue from the auto save. */
bool tico_was_restarted(void);

/* Queues this NRO with the same game for when this process exits. */
bool tico_queue_restart(void);

/* True when started without a game: the host shows the game list. */
bool tico_library_mode(void);

/* Queues this NRO with a game chosen from the game list; Exit Game in it
 * comes back to the list. */
bool tico_queue_library_game(const char *rom);

/* Queues tico to be loaded with --resume when this process exits. */
bool tico_queue_return(void);

/* Appends to TICO_LOG_PATH. */
void tico_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
#define TICO_LOG_PATH DATA_ROOT "/tico.log"

#endif
