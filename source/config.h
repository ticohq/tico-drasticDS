/* Shared build and SD-card layout for DrasticDS_nx. */
#ifndef DRASTIC_NX_CONFIG_H
#define DRASTIC_NX_CONFIG_H

/* libdrastic_arm64.so spans just under 64 MiB including BSS. */
#define SO_REGION_MB 96

/* Leave room outside newlib's heap for NVK images, command buffers and WSI. */
#define GPU_RESERVE_MB 512

#define SO_NAME "libdrastic_arm64.so"

#ifdef DRASTIC_TICO
/* tico build: Drastic's own files live under tico's NDS system folder, while
 * cartridge saves and save states use tico's shared per-system folders. */
#define TICO_NDS_DIR    "/tico/system/nds"
#define DATA_ROOT       TICO_NDS_DIR "/drastic"
#else
#define DATA_ROOT       "/switch/drastic"
#endif
#define SYSTEM_DIR      DATA_ROOT "/system"
#define USER_DIR        DATA_ROOT "/user"
#define CACHE_DIR       DATA_ROOT "/cache"
#define UNZIP_CACHE_DIR DATA_ROOT "/unzip_cache"
#define GAMES_DIR       DATA_ROOT "/games"
#define CHEATS_DIR      DATA_ROOT "/cheats"
#define SCRIPTS_DIR     DATA_ROOT "/scripts"
#define SHADERS_DIR     DATA_ROOT "/shaders"
#define SLOT2_DIR       DATA_ROOT "/slot2"
#define MICROPHONE_DIR  DATA_ROOT "/microphone"
#ifdef DRASTIC_TICO
#define SAVESTATES_DIR  "/tico/states/nds"
#define BACKUPS_DIR     "/tico/saves/nds"
#else
#define SAVESTATES_DIR  USER_DIR "/savestates"
#define BACKUPS_DIR     USER_DIR "/backup"
#endif
#define PREFS_NAME      "drastic.ini"
#define PREFS_PATH      DATA_ROOT "/" PREFS_NAME
#define DEFAULT_ROM_PATH GAMES_DIR "/game.nds"

#define DRASTIC_APK_VERSION_CODE 109
#define ANDROID_SDK_INT 30
#define ANDROID_MANUFACTURER "Nintendo"
#define ANDROID_MODEL "Switch"
#define ANDROID_DEVICE_NAME "Nintendo Switch"

extern int screen_width;
extern int screen_height;
extern int panel_width;
extern int panel_height;

#endif
