#ifndef DRASTIC_NX_ARCHIVE_LOADER_H
#define DRASTIC_NX_ARCHIVE_LOADER_H

#include <stddef.h>

/* Extract the single Nintendo DS image in a ZIP into DraStic's persistent
 * unzip cache.  A validated cache hit only reads the ZIP central directory;
 * the ROM payload is not decompressed again. */
int drastic_zip_prepare(const char *archive_path, char *rom_path,
                        size_t rom_path_size, char *error,
                        size_t error_size);

/* The same for the single Nintendo DS image in a .7z. */
int drastic_7z_prepare(const char *archive_path, char *rom_path,
                       size_t rom_path_size, char *error,
                       size_t error_size);

#endif
