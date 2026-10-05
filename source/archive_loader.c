#include "archive_loader.h"

#include <errno.h>
#include <fcntl.h>
#include <archive.h>
#include <archive_entry.h>
#include <minizip/unzip.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"

#define ZIP_NAME_LIMIT (64u * 1024u)
#define ZIP_COPY_BUFFER (256u * 1024u)
#define DS_ROM_SIZE_LIMIT (512ull * 1024ull * 1024ull)

typedef struct {
  char magic[8];
  uint64_t archive_size;
  int64_t archive_mtime;
  uint64_t archive_path_hash;
  uint64_t entry_name_hash;
  uint64_t rom_size;
  uint32_t rom_crc;
  uint32_t reserved;
} ZipCacheInfo;

static const char zip_cache_magic[8] = {'D', 'R', 'N', 'X', 'Z', 'I', 'P', '1'};

static void set_error(char *output, size_t output_size, const char *format,
                      ...) {
  if (!output || !output_size) return;
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(output, output_size, format, arguments);
  va_end(arguments);
}

static uint64_t fnv1a(const char *text) {
  uint64_t hash = UINT64_C(14695981039346656037);
  if (!text) return hash;
  while (*text) {
    hash ^= (unsigned char)*text++;
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static int nds_name(const char *name) {
  if (!name || !*name) return 0;
  const size_t length = strlen(name);
  if (!length || name[length - 1] == '/' || name[length - 1] == '\\') return 0;
  const char *extension = strrchr(name, '.');
  return extension && !strcasecmp(extension, ".nds");
}

static void cache_stem(const char *archive_path, char *output,
                       size_t output_size) {
  const char *name = strrchr(archive_path, '/');
  const char *backslash = strrchr(archive_path, '\\');
  if (!name || (backslash && backslash > name)) name = backslash;
  name = name ? name + 1 : archive_path;

  size_t length = strlen(name);
  const char *extension = strrchr(name, '.');
  if (extension && (!strcasecmp(extension, ".zip") ||
                    !strcasecmp(extension, ".7z")))
    length = (size_t)(extension - name);
  if (length > 180) length = 180;

  size_t written = 0;
  for (size_t index = 0; index < length && written + 1 < output_size; index++) {
    const unsigned char character = (unsigned char)name[index];
    const int safe = (character >= 'a' && character <= 'z') ||
                     (character >= 'A' && character <= 'Z') ||
                     (character >= '0' && character <= '9') ||
                     character == ' ' || character == '-' || character == '_' ||
                     character == '.' || character == '(' || character == ')';
    output[written++] = safe ? (char)character : '_';
  }
  while (written && (output[written - 1] == ' ' || output[written - 1] == '.'))
    written--;
  if (!written) {
    snprintf(output, output_size, "archive");
    return;
  }
  output[written] = '\0';
}

static int replace_cache_file(const char *temporary, const char *destination) {
  if (rename(temporary, destination) == 0) return 1;
  if (errno != EEXIST && errno != ENOTEMPTY) return 0;
  if (remove(destination) != 0 && errno != ENOENT) return 0;
  return rename(temporary, destination) == 0;
}

static int read_cache_info(const char *path, ZipCacheInfo *info) {
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  const int valid = fread(info, sizeof(*info), 1, file) == 1 &&
                    fgetc(file) == EOF && !ferror(file);
  fclose(file);
  return valid;
}

static int write_cache_info(const char *path, const ZipCacheInfo *info,
                            char *error, size_t error_size) {
  char temporary[1100];
  if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
      (int)sizeof(temporary)) {
    set_error(error, error_size, "ZIP cache metadata path is too long.");
    return 0;
  }
  FILE *file = fopen(temporary, "wb");
  if (!file) {
    set_error(error, error_size, "Could not create ZIP cache metadata: %s",
              strerror(errno));
    return 0;
  }
  int valid = fwrite(info, sizeof(*info), 1, file) == 1 &&
              fflush(file) == 0 && fsync(fileno(file)) == 0;
  if (fclose(file) != 0) valid = 0;
  if (!valid || !replace_cache_file(temporary, path)) {
    const int saved_errno = errno;
    remove(temporary);
    set_error(error, error_size, "Could not finalize ZIP cache metadata: %s",
              strerror(saved_errno ? saved_errno : EIO));
    return 0;
  }
  return 1;
}

static int cache_matches(const char *rom_path, const char *metadata_path,
                         const ZipCacheInfo *expected) {
  ZipCacheInfo stored;
  struct stat rom_status;
  return read_cache_info(metadata_path, &stored) &&
         !memcmp(stored.magic, zip_cache_magic, sizeof(stored.magic)) &&
         stored.archive_size == expected->archive_size &&
         stored.archive_mtime == expected->archive_mtime &&
         stored.archive_path_hash == expected->archive_path_hash &&
         stored.entry_name_hash == expected->entry_name_hash &&
         stored.rom_size == expected->rom_size &&
         stored.rom_crc == expected->rom_crc &&
         stat(rom_path, &rom_status) == 0 && S_ISREG(rom_status.st_mode) &&
         (uint64_t)rom_status.st_size == expected->rom_size;
}

int drastic_zip_prepare(const char *archive_path, char *rom_path,
                        size_t rom_path_size, char *error,
                        size_t error_size) {
  if (error && error_size) error[0] = '\0';
  if (!archive_path || !*archive_path || !rom_path || !rom_path_size) {
    set_error(error, error_size, "Invalid ZIP launch path.");
    return 0;
  }

  struct stat archive_status;
  if (stat(archive_path, &archive_status) != 0 ||
      !S_ISREG(archive_status.st_mode)) {
    set_error(error, error_size, "Could not read ZIP archive: %s",
              strerror(errno));
    return 0;
  }

  unzFile archive = unzOpen64(archive_path);
  if (!archive) {
    set_error(error, error_size, "The selected file is not a readable ZIP archive.");
    return 0;
  }

  unz64_file_pos selected_position;
  unz_file_info64 selected_info;
  char *selected_name = NULL;
  int selected_count = 0;
  int result = unzGoToFirstFile(archive);
  while (result == UNZ_OK) {
    unz_file_info64 info;
    if (unzGetCurrentFileInfo64(archive, &info, NULL, 0, NULL, 0, NULL, 0) !=
        UNZ_OK) {
      set_error(error, error_size, "Could not read the ZIP directory.");
      goto failure;
    }
    if (info.size_filename > ZIP_NAME_LIMIT) {
      set_error(error, error_size, "A ZIP entry name is unreasonably long.");
      goto failure;
    }
    char *name = malloc((size_t)info.size_filename + 1);
    if (!name) {
      set_error(error, error_size, "Not enough memory to inspect the ZIP archive.");
      goto failure;
    }
    if (unzGetCurrentFileInfo64(archive, &info, name,
                                (uLong)info.size_filename + 1,
                                NULL, 0, NULL, 0) != UNZ_OK) {
      free(name);
      set_error(error, error_size, "Could not read a ZIP entry name.");
      goto failure;
    }
    name[info.size_filename] = '\0';
    if (nds_name(name)) {
      selected_count++;
      if (selected_count > 1) {
        free(name);
        set_error(error, error_size,
                  "The ZIP contains more than one .nds game. Keep one ROM per archive.");
        goto failure;
      }
      if (unzGetFilePos64(archive, &selected_position) != UNZ_OK) {
        free(name);
        set_error(error, error_size, "Could not locate the NDS entry in the ZIP.");
        goto failure;
      }
      selected_info = info;
      selected_name = name;
    } else {
      free(name);
    }
    result = unzGoToNextFile(archive);
  }
  if (result != UNZ_END_OF_LIST_OF_FILE) {
    set_error(error, error_size, "The ZIP directory is damaged.");
    goto failure;
  }
  if (!selected_count || !selected_name) {
    set_error(error, error_size, "The ZIP does not contain an .nds game.");
    goto failure;
  }
  if (!selected_info.uncompressed_size ||
      selected_info.uncompressed_size > DS_ROM_SIZE_LIMIT) {
    set_error(error, error_size,
              "The archived ROM size is invalid (%llu bytes).",
              (unsigned long long)selected_info.uncompressed_size);
    goto failure;
  }
  if (selected_info.flag & 1) {
    set_error(error, error_size, "Password-protected ZIP games are not supported.");
    goto failure;
  }

  char stem[192];
  cache_stem(archive_path, stem, sizeof(stem));
  const size_t stem_length = strlen(stem);
  const int stem_has_nds = stem_length >= 4 &&
                           !strcasecmp(stem + stem_length - 4, ".nds");
  if (snprintf(rom_path, rom_path_size, "%s/%s%s", UNZIP_CACHE_DIR, stem,
               stem_has_nds ? "" : ".nds") >= (int)rom_path_size) {
    set_error(error, error_size, "The ZIP cache path is too long.");
    goto failure;
  }
  char metadata_path[1100];
  if (snprintf(metadata_path, sizeof(metadata_path), "%s.nxzip", rom_path) >=
      (int)sizeof(metadata_path)) {
    set_error(error, error_size, "The ZIP cache metadata path is too long.");
    goto failure;
  }

  ZipCacheInfo expected = {0};
  memcpy(expected.magic, zip_cache_magic, sizeof(expected.magic));
  expected.archive_size = (uint64_t)archive_status.st_size;
  expected.archive_mtime = (int64_t)archive_status.st_mtime;
  expected.archive_path_hash = fnv1a(archive_path);
  expected.entry_name_hash = fnv1a(selected_name);
  expected.rom_size = (uint64_t)selected_info.uncompressed_size;
  expected.rom_crc = (uint32_t)selected_info.crc;
  if (cache_matches(rom_path, metadata_path, &expected)) {
    free(selected_name);
    unzClose(archive);
    return 1;
  }

  if (unzGoToFilePos64(archive, &selected_position) != UNZ_OK ||
      unzOpenCurrentFile(archive) != UNZ_OK) {
    set_error(error, error_size,
              "Could not open the NDS entry. Its ZIP compression method may be unsupported.");
    goto failure;
  }

  char temporary_path[1100];
  if (snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", rom_path) >=
      (int)sizeof(temporary_path)) {
    unzCloseCurrentFile(archive);
    set_error(error, error_size, "The temporary ZIP cache path is too long.");
    goto failure;
  }
  FILE *output = fopen(temporary_path, "wb");
  if (!output) {
    unzCloseCurrentFile(archive);
    set_error(error, error_size, "Could not create the extracted ROM cache: %s",
              strerror(errno));
    goto failure;
  }
  void *buffer = malloc(ZIP_COPY_BUFFER);
  if (!buffer) {
    fclose(output);
    remove(temporary_path);
    unzCloseCurrentFile(archive);
    set_error(error, error_size, "Not enough memory to extract the ZIP archive.");
    goto failure;
  }

  uint64_t copied = 0;
  int read_result;
  int write_failed = 0;
  while ((read_result = unzReadCurrentFile(archive, buffer,
                                           ZIP_COPY_BUFFER)) > 0) {
    if (fwrite(buffer, 1, (size_t)read_result, output) !=
        (size_t)read_result) {
      write_failed = 1;
      break;
    }
    copied += (uint64_t)read_result;
  }
  free(buffer);
  const int close_entry_result = unzCloseCurrentFile(archive);
  int output_valid = !write_failed && read_result == 0 &&
                     close_entry_result == UNZ_OK &&
                     copied == expected.rom_size && fflush(output) == 0 &&
                     fsync(fileno(output)) == 0;
  if (fclose(output) != 0) output_valid = 0;
  if (!output_valid || !replace_cache_file(temporary_path, rom_path)) {
    const int saved_errno = errno;
    remove(temporary_path);
    set_error(error, error_size,
              close_entry_result == UNZ_CRCERROR
                  ? "The archived ROM failed its ZIP CRC check."
                  : "Could not extract the archived ROM: %s",
              strerror(saved_errno ? saved_errno : EIO));
    goto failure;
  }
  if (!write_cache_info(metadata_path, &expected, error, error_size)) {
    remove(rom_path);
    goto failure;
  }

  free(selected_name);
  unzClose(archive);
  return 1;

failure:
  free(selected_name);
  unzClose(archive);
  return 0;
}

/* Opens a .7z for reading, or NULL. */
static struct archive *open_7z(const char *archive_path) {
  struct archive *archive = archive_read_new();
  if (!archive) return NULL;
  archive_read_support_format_7zip(archive);
  archive_read_support_filter_all(archive);
  if (archive_read_open_filename(archive, archive_path, 256 * 1024) !=
      ARCHIVE_OK) {
    archive_read_free(archive);
    return NULL;
  }
  return archive;
}

int drastic_7z_prepare(const char *archive_path, char *rom_path,
                       size_t rom_path_size, char *error,
                       size_t error_size) {
  if (error && error_size) error[0] = '\0';
  if (!archive_path || !*archive_path || !rom_path || !rom_path_size) {
    set_error(error, error_size, "Invalid 7z launch path.");
    return 0;
  }
  struct stat archive_status;
  if (stat(archive_path, &archive_status) != 0 ||
      !S_ISREG(archive_status.st_mode)) {
    set_error(error, error_size, "Could not read 7z archive: %s",
              strerror(errno));
    return 0;
  }

  /* First pass: the archive must hold exactly one .nds. */
  struct archive *archive = open_7z(archive_path);
  if (!archive) {
    set_error(error, error_size, "The selected file is not a readable 7z archive.");
    return 0;
  }
  char selected_name[1024] = "";
  int64_t selected_size = 0;
  int selected_count = 0;
  struct archive_entry *entry;
  int result;
  while ((result = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
    const char *name = archive_entry_pathname(entry);
    if (archive_entry_filetype(entry) == AE_IFREG && nds_name(name)) {
      if (++selected_count > 1) break;
      snprintf(selected_name, sizeof(selected_name), "%s", name);
      selected_size = archive_entry_size(entry);
    }
    archive_read_data_skip(archive);
  }
  const int listed = result == ARCHIVE_EOF || selected_count > 1;
  archive_read_free(archive);
  if (!listed) {
    set_error(error, error_size, "The 7z directory is damaged.");
    return 0;
  }
  if (selected_count > 1) {
    set_error(error, error_size,
              "The 7z contains more than one .nds game. Keep one ROM per archive.");
    return 0;
  }
  if (!selected_count) {
    set_error(error, error_size, "The 7z does not contain an .nds game.");
    return 0;
  }
  if (selected_size <= 0 || (uint64_t)selected_size > DS_ROM_SIZE_LIMIT) {
    set_error(error, error_size, "The archived ROM size is invalid (%lld bytes).",
              (long long)selected_size);
    return 0;
  }

  char stem[192];
  cache_stem(archive_path, stem, sizeof(stem));
  const size_t stem_length = strlen(stem);
  const int stem_has_nds = stem_length >= 4 &&
                           !strcasecmp(stem + stem_length - 4, ".nds");
  if (snprintf(rom_path, rom_path_size, "%s/%s%s", UNZIP_CACHE_DIR, stem,
               stem_has_nds ? "" : ".nds") >= (int)rom_path_size) {
    set_error(error, error_size, "The 7z cache path is too long.");
    return 0;
  }
  char metadata_path[1100];
  if (snprintf(metadata_path, sizeof(metadata_path), "%s.nxzip", rom_path) >=
      (int)sizeof(metadata_path)) {
    set_error(error, error_size, "The 7z cache metadata path is too long.");
    return 0;
  }
  /* 7z keeps no CRC libarchive hands back cheaply: the archive's size and
   * time stand for its contents */
  ZipCacheInfo expected = {0};
  memcpy(expected.magic, zip_cache_magic, sizeof(expected.magic));
  expected.archive_size = (uint64_t)archive_status.st_size;
  expected.archive_mtime = (int64_t)archive_status.st_mtime;
  expected.archive_path_hash = fnv1a(archive_path);
  expected.entry_name_hash = fnv1a(selected_name);
  expected.rom_size = (uint64_t)selected_size;
  if (cache_matches(rom_path, metadata_path, &expected)) return 1;

  /* Second pass: unpack it. */
  archive = open_7z(archive_path);
  if (!archive) {
    set_error(error, error_size, "The selected file is not a readable 7z archive.");
    return 0;
  }
  int found = 0;
  while ((result = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
    if (archive_entry_filetype(entry) == AE_IFREG &&
        !strcmp(archive_entry_pathname(entry), selected_name)) {
      found = 1;
      break;
    }
    archive_read_data_skip(archive);
  }
  if (!found) {
    archive_read_free(archive);
    set_error(error, error_size, "Could not locate the NDS entry in the 7z.");
    return 0;
  }
  char temporary_path[1100];
  if (snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", rom_path) >=
      (int)sizeof(temporary_path)) {
    archive_read_free(archive);
    set_error(error, error_size, "The temporary 7z cache path is too long.");
    return 0;
  }
  FILE *output = fopen(temporary_path, "wb");
  void *buffer = output ? malloc(ZIP_COPY_BUFFER) : NULL;
  if (!output || !buffer) {
    const int saved_errno = errno;
    if (output) fclose(output);
    remove(temporary_path);
    archive_read_free(archive);
    set_error(error, error_size, "Could not create the extracted ROM cache: %s",
              strerror(saved_errno ? saved_errno : ENOMEM));
    return 0;
  }
  uint64_t copied = 0;
  la_ssize_t read_result;
  int write_failed = 0;
  while ((read_result = archive_read_data(archive, buffer, ZIP_COPY_BUFFER)) > 0) {
    if (fwrite(buffer, 1, (size_t)read_result, output) != (size_t)read_result) {
      write_failed = 1;
      break;
    }
    copied += (uint64_t)read_result;
  }
  free(buffer);
  archive_read_free(archive);
  int output_valid = !write_failed && read_result == 0 &&
                     copied == expected.rom_size && fflush(output) == 0 &&
                     fsync(fileno(output)) == 0;
  if (fclose(output) != 0) output_valid = 0;
  if (!output_valid || !replace_cache_file(temporary_path, rom_path)) {
    const int saved_errno = errno;
    remove(temporary_path);
    set_error(error, error_size, "Could not extract the archived ROM: %s",
              strerror(saved_errno ? saved_errno : EIO));
    return 0;
  }
  if (!write_cache_info(metadata_path, &expected, error, error_size)) {
    remove(rom_path);
    return 0;
  }
  return 1;
}
