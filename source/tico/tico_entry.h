/* tico frontend entry for the Drastic host.
 *
 * tico chainloads tico-drastic.nro with the ROM path in argv[1] and the game's
 * display title in argv[2]; on exit the host chainloads back to tico with
 * --resume. Started without a ROM, the first game in /switch/tico-drastic is
 * used so the host can be tested without tico. */
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

/* Queues tico to be loaded with --resume when this process exits. */
bool tico_queue_return(void);

/* Appends to TICO_LOG_PATH. */
void tico_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
#define TICO_LOG_PATH DATA_ROOT "/tico.log"

#endif
