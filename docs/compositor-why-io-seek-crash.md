# Compositor Boot Crash: stdlib seek on why_io FILE handles

## Summary

When the autorotate feature was added to the compositor, it read and wrote
`config.json` using `why_fopen`/`why_fread`/`why_fclose` — but the seek and
position calls were accidentally written as the standard C library functions
`fseek`, `ftell`, and `rewind` instead of the `why_io` equivalents
`why_fseek`, `why_ftell`, and `why_rewind`.

The `why_io` VFS layer does not populate the seek/tell ops in the underlying
FILE structure. Calling `fseek` on a `why_fopen` handle therefore jumps through
a null or uninitialised function pointer, which on RISC-V (ESP32-P4) is seen
by the CPU as an illegal instruction. The result is a boot-time Guru Meditation
panic with a yellow screen before the launcher even starts.

---

## Background

BadgeVMS has its own virtual filesystem layer (`why_io`) that wraps the ESP-IDF
VFS. It exposes a `FILE *`-compatible API under names like `why_fopen`,
`why_fclose`, `why_fread`, `why_fwrite`, `why_fseek`, `why_ftell`, and
`why_rewind`. These functions must be used together: a file opened with
`why_fopen` carries a FILE handle whose internal ops table is populated by the
`why_io` VFS, not by the standard C library. The standard `fseek`/`ftell`/
`rewind` look up a seek op in a different part of the FILE struct that the
`why_io` layer never fills in.

---

## The Bug

`compositor_init` and `compositor_set_autorotate` were added to read and update
the `autorotate` field in `APPS:[badgevms_launcher]config.json`. The read path
used the standard C seek functions to size the file before allocating a buffer:

```c
FILE *cfg_f = why_fopen("APPS:[badgevms_launcher]config.json", "r");
if (cfg_f) {
    fseek(cfg_f, 0, SEEK_END);   // <-- wrong: stdlib on a why_io handle
    long cfg_sz = ftell(cfg_f);  // <-- wrong
    rewind(cfg_f);               // <-- wrong
    ...
```

The same pattern appeared in `compositor_set_autorotate`.

### Crash mechanics

On RISC-V, calling through a null function pointer does not fault at the call
site — the CPU loads the value (0 or garbage) into the program counter and then
tries to execute whatever is at that address. If that address is unmapped or
contains data rather than valid instructions, the CPU raises an "Illegal
instruction" exception (MCAUSE = 0x2).

The panic log confirmed this:

```
Guru Meditation Error: Core 0 panic'ed (Illegal instruction). Exception was unhandled.

MEPC    : 0x480102b0
...
A4      : 0x480102b0
S5      : 0x480102b0
T6      : 0x480102b0
```

MEPC, A4, S5, and T6 all holding the same value is the classic signature of a
corrupt or null function pointer: the bad address was loaded from the FILE ops
table, placed into multiple registers as part of the call sequence, and then
jumped to. The address `0x480102b0` sits in the virtual address range reserved
for dynamically-loaded app ELFs — not executable at the time the compositor
initialises.

The `why_bufio` symbols visible in other registers (`__why_bufio_seek`,
`__why_bufio_close`, `__why_bufio_get`, `__why_bufio_put`) confirmed that
`why_io` file I/O was in flight when the crash occurred.

---

## The Fix

Replace all three stdlib seek calls with their `why_io` counterparts in both
`compositor_init` and `compositor_set_autorotate`:

```c
// Before
fseek(cfg_f, 0, SEEK_END);
long cfg_sz = ftell(cfg_f);
rewind(cfg_f);

// After
why_fseek(cfg_f, 0, SEEK_END);
long cfg_sz = why_ftell(cfg_f);
why_rewind(cfg_f);
```

The rule is simple: every function that touches a `why_fopen` handle must come
from `why_io.h`. Never mix `why_fopen` with stdlib `fseek`/`ftell`/`rewind`/
`fread`/`fwrite`/`fclose`.

---

## How to Spot This Class of Bug

- A boot-time "Illegal instruction" panic with MEPC equal to a value also found
  in several general-purpose registers is almost always a null or corrupt
  function pointer.
- If `why_bufio` symbols appear in the register dump, the crash happened inside
  or immediately after a `why_io` file operation.
- Check every `why_fopen` call site: if any of `fseek`, `ftell`, `rewind`,
  `fread`, `fwrite`, or `fclose` appear on the same handle, they must be
  replaced with their `why_` prefixed equivalents.
