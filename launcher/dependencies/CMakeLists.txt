cmake_minimum_required(VERSION 3.22)
project(DrasticDSLauncherDependencies LANGUAGES C CXX)

include(FetchContent)

set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBKRB5 OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libsmb2
  GIT_REPOSITORY https://github.com/ITotalJustice/libsmb2.git
  GIT_TAG 867beea093f2863dfddea01945204f724afd6c45)
FetchContent_MakeAvailable(libsmb2)
target_compile_options(smb2 PRIVATE -UNEED_READV -UNEED_WRITEV)
target_compile_definitions(smb2 PRIVATE AES128_ECB_encrypt=smb2_AES128_ECB_encrypt)

# Enable NTFS on its own: USBHSFS_GPL would also pull in EXT2/3/4.
set(USBHSFS_GPL OFF CACHE BOOL "" FORCE)
set(USBHSFS_NTFS ON CACHE BOOL "" FORCE)
set(USBHSFS_EXT4 OFF CACHE BOOL "" FORCE)
set(USBHSFS_SXOS_DISABLE ON CACHE BOOL "" FORCE)
set(USBHSFS_EXAMPLES OFF CACHE BOOL "" FORCE)

find_library(ntfs_3g_lib ntfs-3g)
find_path(ntfs_3g_inc ntfs-3g/volume.h)
if(NOT ntfs_3g_lib OR NOT ntfs_3g_inc)
  message(FATAL_ERROR
    "Switch NTFS support requires switch-ntfs-3g. Install it with "
    "'dkp-pacman -S switch-ntfs-3g' (or 'pacman -S switch-ntfs-3g' in devkitPro MSYS2).")
endif()

set(_drastic_libusbhsfs_pinned_revision 625269b7725a6e2a3f2724e8d45b602c1b20ead5)
FetchContent_Declare(libusbhsfs
  GIT_REPOSITORY https://github.com/ITotalJustice/libusbhsfs.git
  GIT_TAG ${_drastic_libusbhsfs_pinned_revision})
FetchContent_MakeAvailable(libusbhsfs)

# Reuse the hardware-tested Dolphin-NX UASP transport on the exact pinned
# libusbhsfs tree.  Reconfiguration is idempotent and refuses an unexpected
# dependency revision instead of applying protocol patches to unknown code.
find_package(Git REQUIRED)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}" rev-parse HEAD
  WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
  RESULT_VARIABLE _drastic_libusbhsfs_revision_result
  OUTPUT_VARIABLE _drastic_libusbhsfs_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _drastic_libusbhsfs_revision_result EQUAL 0 OR
   NOT "${_drastic_libusbhsfs_revision}" STREQUAL "${_drastic_libusbhsfs_pinned_revision}")
  message(FATAL_ERROR "libusbhsfs source is not the pinned ${_drastic_libusbhsfs_pinned_revision} revision")
endif()

set(_drastic_libusbhsfs_patches
  "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs/0001-add-uasp-transport-hooks.patch"
  "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs/0002-fallback-to-bot-interface.patch"
  "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs/0003-cleanup-after-uasp-reset.patch")
foreach(_drastic_libusbhsfs_patch IN LISTS _drastic_libusbhsfs_patches)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
            apply --ignore-space-change --check "${_drastic_libusbhsfs_patch}"
    WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
    RESULT_VARIABLE _drastic_libusbhsfs_patch_check OUTPUT_QUIET ERROR_QUIET)
  if(_drastic_libusbhsfs_patch_check EQUAL 0)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
              apply --ignore-space-change "${_drastic_libusbhsfs_patch}"
      WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
      RESULT_VARIABLE _drastic_libusbhsfs_patch_result)
    if(NOT _drastic_libusbhsfs_patch_result EQUAL 0)
      message(FATAL_ERROR "Failed to apply ${_drastic_libusbhsfs_patch}")
    endif()
  else()
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
              apply --ignore-space-change --reverse --check "${_drastic_libusbhsfs_patch}"
      WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
      RESULT_VARIABLE _drastic_libusbhsfs_patch_reverse_check OUTPUT_QUIET ERROR_QUIET)
    if(NOT _drastic_libusbhsfs_patch_reverse_check EQUAL 0)
      message(FATAL_ERROR "The pinned libusbhsfs source does not match ${_drastic_libusbhsfs_patch}")
    endif()
  endif()
endforeach()

target_sources(libusbhsfs PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs/usbhsfs_uasp.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs/usbhsfs_uasp.h")
target_include_directories(libusbhsfs PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs"
  "${libusbhsfs_SOURCE_DIR}/source")
target_compile_definitions(libusbhsfs PRIVATE get_fattime=usbhsfs_get_fattime)
