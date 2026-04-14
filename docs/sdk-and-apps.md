# BadgeVMS SDK and Application Development

## What apps are

BadgeVMS apps are **position-independent ELF shared objects** for RISC-V 32-bit:
- Target arch: `rv32imafc_zicsr_zifencei`
- ABI: `ilp32f`
- Entry point: `main`
- Built with `-fPIC -fvisibility=hidden -nostartfiles -nostdlib -shared`
- All symbols hidden except `main`; no `dlopen`; all deps statically linked

## Build SDK

```bash
idf.py sdk
# Output: sdk_dist/ with headers and .a libs
```

## Compile a single app

```bash
riscv32-esp-elf-gcc -O2 -fPIC -fdata-sections -ffunction-sections -flto \
  -fno-builtin -fno-builtin-function -fno-jump-tables -fno-tree-switch-conversion \
  -fstrict-volatile-bitfields -fvisibility=hidden -g3 -mabi=ilp32f \
  -march=rv32imafc_zicsr_zifencei -nostartfiles -nostdlib -shared \
  -Wl,--strip-debug -Wl,--gc-sections -e main --sysroot sdk_dist -isystem sdk_dist/include \
  hello.c -o hello.elf
```

Also works with `riscv64-linux-gnu-gcc` (Fedora 42+). When linking a `.a` file, add:
`-Wl,--exclude-libs,libmylib.a` to avoid exposing unwanted symbols.

## Available SDK APIs (headers in `badgevms/include/badgevms/`)

| Header | What it provides |
|--------|-----------------|
| `compositor.h` | Windows: create/destroy/resize, get framebuffer, present, poll events |
| `framebuffer.h` | `framebuffer_t` struct, `rgb888_to_rgb565()` helper |
| `pixel_formats.h` | `pixel_format_t` enum (RGB565, RGB888, etc.) |
| `event.h` | `event_t` (keyboard, window events) |
| `keyboard.h` | Key codes and keyboard event structs |
| `keymap_us.h` | US keyboard layout key definitions |
| `device.h` | `device_get(name)`, device type structs, all device vtable types |
| `process.h` | `process_create(path, stack, argc, argv)` → pid_t |
| `pathfuncs.h` | VMS path parsing/manipulation utilities |
| `application.h` | App registry API (create/get/list/launch/destroy applications) |
| `ota.h` | OTA update session API |
| `wifi.h` | WiFi connect/disconnect/status |
| `misc_funcs.h` | Miscellaneous utility functions |
| `curl/curl.h` | libcurl (HTTPS supported via esp-tls) |

Plus full newlib libc + SDL3 and SDL2.

## Example apps (`sdk_apps/`)

| App | Description |
|-----|-------------|
| `framebuffer_test` | Direct windowing + keyboard input (no SDL) |
| `sdl_test` | SDL3 windowing + input |
| `sdl2_test` | SDL2 windowing + input |
| `curl_test` | HTTP(S) with libcurl |
| `thread_test` | Thread creation and IPC |
| `process_test` | Spawning child processes |
| `socket_test` | BSD sockets |
| `wifi_test` | WiFi connection |
| `hello` | Minimal hello world |
| `readdir_test` | Filesystem directory traversal |
| `bme690_test` | Gas sensor reading |
| `bmi270_test` | IMU sensor reading |
| `hardware_test` | General hardware test |
| `memtester` | Memory stress test |
| `ota_wifi_update` | OTA update flow |
| `doomgeneric` | Full Doom port (best reference for complex apps) |
| `badgevms_launcher` | Official launcher UI |
| `badgevms_settings` | Settings app |
| `why2025_namebadge` | Name badge display |
| `why2025_ota` | WHY2025 OTA update client |
| `why2025_sponsors` | Sponsor display (runs once at boot) |
| `appdb_test` | Application registry API test |
| `bench_basic_a/b` | Benchmarks |

**Best reference for a full app**: `sdk_apps/doomgeneric/doomgeneric/doomgeneric_badgevms.c` — shows framebuffers, scaling, window handling, input.

## Application Registry API (`include/badgevms/application.h`)

Apps are registered in `APPS:` (backed by JSON files on flash/SD):

```c
// Create app entry (fails if already exists)
application_t *app = application_create(uid, name, author, version, interpreter, source);

// Set binary path (relative to app's installed_path)
application_set_binary_path(app, "myapp.elf");

// Launch app (creates a process)
pid_t pid = application_launch(uid);

// List all installed apps
application_list_handle h = application_list(&first_app);
application_t *app;
while ((app = application_list_get_next(h))) { ... }
application_list_close(h);

// Get app
application_t *app = application_get(uid);
application_free(app);

// Destroy app (removes files)
application_destroy(app);
```

App metadata stored as: `APPS:uid.json` (JSON file)  
App files stored in: `APPS:[uid]/` directory

## Threading

```c
// From process.h — create a new process
pid_t process_create(path, stack_size, argc, argv);

// From task.h — create a thread sharing same heap/files
pid_t thread_create(entry_fn, user_data, stack_size);

// Wait for child to exit
pid_t wait(bool block, uint32_t timeout_ms);
```

## Stack size defaults

- Minimum stack: `MIN_STACK_SIZE` = 16384 bytes
- Default in init.toml: 8192 bytes (bumped to MIN_STACK_SIZE automatically)
- Complex apps (SDL, Doom): use 16384–65536

## Configuring startup apps (init.toml)

The `flash_storage/skel/init.toml` is embedded in the firmware binary via CMake and **always overwrites** `FLASH0:init.toml` on boot. Edits to the app launch config go here.

Default boot app: `why2025_sponsors` (run_once, shows sponsor screen on first boot).
