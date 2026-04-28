#include "../shared/rom_browser.h"

#include <badgevms/compositor.h>
#include <badgevms/process.h>

#include <stdbool.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    window_handle_t win = window_create(
        "NES Launcher",
        (window_size_t){720, 720},
        WINDOW_FLAG_DOUBLE_BUFFERED | WINDOW_FLAG_MAXIMIZED
    );
    framebuffer_t *fb = window_framebuffer_create(
        win, (window_size_t){720, 720}, BADGEVMS_PIXELFORMAT_RGB565
    );

    const char *exts[] = {".nes", NULL};
    rom_browser_config_t cfg = {
        .rom_dir     = "SD0:[ROMS.NES]",
        .extensions  = exts,
        .title       = "NES Launcher",
        .window      = win,
        .framebuffer = fb,
    };

    while (true) {
        char *rom = rom_browser_run(&cfg);
        if (!rom) break;
        const char *args[] = {"nes_emu.elf", rom};
        process_create("APPS:[nes_emu]nes_emu.elf", 65536, 2, (char **)args);
        wait(true, 0);
        free(rom);
    }

    window_destroy(win);
    return 0;
}
