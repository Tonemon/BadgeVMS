#pragma once

#include <badgevms/compositor.h>
#include <badgevms/framebuffer.h>
#include <stddef.h>

typedef struct {
    const char      *rom_dir;       /* e.g. "SD0:[ROMS.NES]" */
    const char     **extensions;    /* NULL-terminated, e.g. {".nes", NULL} */
    const char      *title;         /* displayed in the title bar */
    window_handle_t  window;
    framebuffer_t   *framebuffer;
} rom_browser_config_t;

/*
 * Show the ROM picker UI. Returns a heap-allocated VMS path of the selected
 * ROM, or NULL if the user pressed ESC. Caller must free() the return value.
 */
char *rom_browser_run(const rom_browser_config_t *cfg);

/*
 * Scan only — no UI. Returns a heap-allocated array of *count
 * heap-allocated path strings sorted alphabetically. Caller frees each
 * string and the array itself. Returns NULL if no ROMs found.
 */
char **rom_browser_scan(const char *rom_dir, const char **extensions,
                        size_t *count);
