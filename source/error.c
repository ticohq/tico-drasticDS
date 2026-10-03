/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "error.h"
#ifdef DRASTIC_TICO
#include "tico/tico_entry.h"
#endif

static int g_graphics_active;

void fatal_error_set_graphics_active(int active) {
  g_graphics_active = active != 0;
}

void fatal_error(const char *fmt, ...) {
  // Format once before switching the display over to the console.
  char msg[1024];
  va_list list;
  va_start(list, fmt);
  vsnprintf(msg, sizeof(msg), fmt, list);
  va_end(list);

  /* consoleInit reclaims VI/framebuffer state and cannot safely run after the
   * Vulkan or EGL renderer owns the display. Return directly to hbmenu
   * instead of turning a handled runtime failure into a second exception. */
  if (g_graphics_active) {
    svcExitProcess();
    __builtin_unreachable();
  }

  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  consoleInit(NULL);

  printf("DrasticDS_nx - fatal error\n\n%s", msg);

  printf("\n\nPress A to exit.");

  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 keys = padGetButtonsDown(&pad);
    if (keys & HidNpadButton_A) break;
  }

  consoleExit(NULL);
#ifdef DRASTIC_TICO
  tico_queue_return();
#endif
  /* The failure was displayed; do not ask hbloader to convert it
     into an opaque non-zero-exit user break. */
  exit(0);
}
