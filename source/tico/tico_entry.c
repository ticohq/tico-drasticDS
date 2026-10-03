#include <switch.h>

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "config.h"
#include "error.h"
#include "prefs.h"
#include "tico/tico_entry.h"

/* where a ROM is picked from when the NRO is started without tico */
#define FALLBACK_ROM_DIR "/switch/tico-drastic"
/* the standalone DrasticDS install, used for files this build does not bundle */
#define STANDALONE_ROOT  "/switch/drastic"
#define BUNDLED_CORE     "romfs:/cores/" SO_NAME
#define BUNDLED_DATABASE "romfs:/res/game_database.xml"

static char display_title[256];

static int file_size(const char *path, long *size) {
  struct stat status;
  if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)) return 0;
  if (size) *size = (long)status.st_size;
  return 1;
}

static int copy_file(const char *from, const char *to) {
  FILE *input = fopen(from, "rb");
  if (!input) return 0;
  char temporary[1100];
  snprintf(temporary, sizeof(temporary), "%s.tmp", to);
  FILE *output = fopen(temporary, "wb");
  if (!output) {
    fclose(input);
    return 0;
  }
  static char buffer[256 * 1024];
  int failed = 0;
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0)
    if (fwrite(buffer, 1, count, output) != count) {
      failed = 1;
      break;
    }
  if (ferror(input)) failed = 1;
  fclose(input);
  if (fclose(output) != 0) failed = 1;
  if (failed) {
    remove(temporary);
    return 0;
  }
  remove(to);
  return rename(temporary, to) == 0;
}

/* Copies the first existing candidate to target unless target already exists.
 * With refresh set, a target whose size differs from the source is replaced. */
static void install_file(const char *target, const char *const *candidates,
                         int refresh) {
  long target_size = 0;
  const int present = file_size(target, &target_size);
  if (present && !refresh) return;
  for (; *candidates; candidates++) {
    long source_size = 0;
    if (!file_size(*candidates, &source_size)) continue;
    if (present && source_size == target_size) return;
    copy_file(*candidates, target);
    return;
  }
}

void tico_make_directories(void) {
  static const char *const directories[] = {
    "/tico", "/tico/system", TICO_NDS_DIR, TICO_NDS_DIR "/debug",
    "/tico/saves", BACKUPS_DIR,
    "/tico/states", SAVESTATES_DIR, DATA_ROOT, DATA_ROOT "/cores",
    SYSTEM_DIR, FALLBACK_ROM_DIR,
  };
  for (unsigned index = 0;
       index < sizeof(directories) / sizeof(*directories); index++)
    mkdir(directories[index], 0777);
}

/* tico keeps the user's NDS dumps in its shared system folder under either
 * the Drastic or the melonDS names; Drastic reads them from SYSTEM_DIR. */
static void stage_system_files(void) {
  static const char *const arm7[] = {
    TICO_NDS_DIR "/nds_bios_arm7.bin", TICO_NDS_DIR "/bios7.bin",
    STANDALONE_ROOT "/system/nds_bios_arm7.bin", NULL,
  };
  static const char *const arm9[] = {
    TICO_NDS_DIR "/nds_bios_arm9.bin", TICO_NDS_DIR "/bios9.bin",
    STANDALONE_ROOT "/system/nds_bios_arm9.bin", NULL,
  };
  static const char *const firmware[] = {
    TICO_NDS_DIR "/nds_firmware.bin", TICO_NDS_DIR "/firmware.bin",
    STANDALONE_ROOT "/system/nds_firmware.bin", NULL,
  };
  static const char *const database[] = {
    BUNDLED_DATABASE, STANDALONE_ROOT "/system/game_database.xml", NULL,
  };
  static const char *const cheats[] = {
    TICO_NDS_DIR "/usrcheat.dat", STANDALONE_ROOT "/system/usrcheat.dat", NULL,
  };
  install_file(SYSTEM_DIR "/nds_bios_arm7.bin", arm7, 0);
  install_file(SYSTEM_DIR "/nds_bios_arm9.bin", arm9, 0);
  install_file(SYSTEM_DIR "/nds_firmware.bin", firmware, 0);
  install_file(SYSTEM_DIR "/game_database.xml", database, 1);
  /* the cheat manager rewrites usrcheat.dat in place to keep toggles */
  install_file(SYSTEM_DIR "/usrcheat.dat", cheats, 0);
}

static const char *select_core(void) {
  static const char *const candidates[] = {
    BUNDLED_CORE, DATA_ROOT "/cores/" SO_NAME,
    STANDALONE_ROOT "/cores/" SO_NAME,
  };
  for (unsigned index = 0;
       index < sizeof(candidates) / sizeof(*candidates); index++)
    if (file_size(candidates[index], NULL)) return candidates[index];
  return candidates[1];
}

static int is_game(const char *name) {
  const char *extension = strrchr(name, '.');
  return extension && (!strcasecmp(extension, ".nds") ||
                       !strcasecmp(extension, ".zip") ||
                       !strcasecmp(extension, ".rar"));
}

/* the alphabetically first game in the fallback folder */
static int find_fallback_rom(char *output, size_t output_size) {
  DIR *directory = opendir(FALLBACK_ROM_DIR);
  if (!directory) return 0;
  char best[512] = "";
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (entry->d_name[0] == '.' || !is_game(entry->d_name)) continue;
    if (!best[0] || strcmp(entry->d_name, best) < 0)
      snprintf(best, sizeof(best), "%s", entry->d_name);
  }
  closedir(directory);
  if (!best[0]) return 0;
  snprintf(output, output_size, "%s/%s", FALLBACK_ROM_DIR, best);
  return 1;
}

static const char *rom_argument(int argc, char **argv) {
  for (int index = 1; index < argc; index++) {
    /* some tico launch paths pass this guard word before the ROM */
    if (!argv[index] || !argv[index][0] || !strcmp(argv[index], "ticoSetup"))
      continue;
    return argv[index];
  }
  return NULL;
}

static void set_display_title(int argc, char **argv, const char *rom) {
  const char *title = NULL;
  if (argc > 2 && argv[2] && argv[2][0]) title = argv[2];
  if (title) {
    snprintf(display_title, sizeof(display_title), "%s", title);
    return;
  }
  const char *name = strrchr(rom, '/');
  name = name ? name + 1 : rom;
  snprintf(display_title, sizeof(display_title), "%s", name);
  char *extension = strrchr(display_title, '.');
  if (extension) *extension = '\0';
}

void tico_prepare(int argc, char **argv) {
  stage_system_files();

  char rom[1024] = "";
  const char *argument = rom_argument(argc, argv);
  if (argument)
    snprintf(rom, sizeof(rom), "%s", argument);
  else if (!find_fallback_rom(rom, sizeof(rom)))
    fatal_error("No game was passed by tico, and none was found in\n"
                "sdmc:" FALLBACK_ROM_DIR "/\n\n"
                "Put a .nds, .zip or .rar there to test without tico.");
  if (rom[0]) prefs_set_disc_path(rom);
  set_display_title(argc, argv, rom);

  prefs_set_string("Wrapper/CoreSo", select_core());
  /* tico owns the return path; the standalone launcher's must not be used */
  prefs_remove("Wrapper/LauncherPath");
  prefs_remove("Wrapper/GameConfigPath");
}

const char *tico_display_title(void) { return display_title; }

void tico_log(const char *format, ...) {
  static FILE *file;
  if (!file) {
    file = fopen(TICO_LOG_PATH, "w");
    if (!file) return;
  }
  va_list arguments;
  va_start(arguments, format);
  vfprintf(file, format, arguments);
  va_end(arguments);
  fflush(file);
}

bool tico_queue_return(void) {
  if (!envHasNextLoad() || !file_size(TICO_LAUNCHER_PATH, NULL)) return false;
  static char arguments[512];
  snprintf(arguments, sizeof(arguments), "%s --resume", TICO_LAUNCHER_PATH);
  return R_SUCCEEDED(envSetNextLoad(TICO_LAUNCHER_PATH, arguments));
}
