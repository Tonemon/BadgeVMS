#pragma once
#include <stddef.h>
#include <string.h>

/*
 * Build "prefix GameName" from a VMS path like SD0:[ROMS.NES]Mario_Bros.nes
 * Result: "[NES] Mario_Bros" — the compositor title bar truncates at 20 chars.
 */
static inline void emu_overlay_label(char *buf, size_t sz,
                                     const char *vms_path,
                                     const char *prefix) {
    const char *name = strrchr(vms_path, ']');
    name = name ? name + 1 : vms_path;
    const char *dot = strrchr(name, '.');
    size_t nlen = dot ? (size_t)(dot - name) : strlen(name);
    size_t plen = strlen(prefix);
    size_t head = plen + 1;  /* "prefix " */
    if (head >= sz) { buf[0] = '\0'; return; }
    memcpy(buf, prefix, plen);
    buf[plen] = ' ';
    size_t avail = sz - head - 1;
    if (nlen > avail) nlen = avail;
    memcpy(buf + head, name, nlen);
    buf[head + nlen] = '\0';
}
