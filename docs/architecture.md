# BadgeVMS Core Architecture

## Boot Sequence (`badgevms/why2025_firmware.c` → `app_main`)

1. `memory_init()` — sets up PSRAM MMU page allocator
2. `task_init()` — creates Zeus (spawner) and Hades (reaper) FreeRTOS tasks; registers kernel task
3. `device_init()` — initialises device registry
4. `logical_names_system_init()` — VMS path alias system
5. `esp_event_loop_create_default()` + `nvs_flash_init()`
6. Register all devices: FLASH0, SD0 (optional), WIFI0, SOCKET0, PANEL0, KEYBOARD0, TT01, I2CBUS0, ORIENTATION0, GAS0
7. `compositor_init("PANEL0", "KEYBOARD0")` — windowing system
8. `run_init()` — kernel init daemon (never returns; reboot if it does)

## Memory Map (PSRAM)

```
SOC_EXTRAM_LOW
  [page allocator metadata — 1 page]
  [kernel SPIRAM heap — 5 MB - 1 page]      KERNEL_HEAP_START/SIZE
  [framebuffer heap — 25 MB]                FRAMEBUFFER_HEAP_START/SIZE
  [unused]
  [guard page]
SOC_EXTRAM_LOW + 32 MB                      VADDR_TASK_START (user app virtual base)
  ... user program virtual address space
SOC_EXTRAM_HIGH
```

Key constants (`badgevms/memory.h`):
- `PHYSMEM_AMOUNT` = 32 MB
- `KERNEL_HEAP_SIZE` = 5 MB − 1 page
- `FRAMEBUFFER_HEAP_SIZE` = 25 MB
- `VADDR_TASK_START` = virtual address where user programs are mapped

Each process gets its own isolated virtual address space. MMU remapping happens on context switch via FreeRTOS task-switch hooks (`remap_task` / `unmap_task`).

Third-party allocators: `dlmalloc` (per-process heap, state stored in `task_thread_t`), `buddy_alloc` (physical page allocator).

## Process/Task Model (`badgevms/task.c`, `task.h`)

### Key FreeRTOS tasks

| Task | Priority | Purpose |
|------|----------|---------|
| Hades | 11 | Reaps dead processes — cleans resources, deallocates pages, notifies parent |
| Zeus | 10 | Spawns new processes — receives `zeus_command_message_t` via queue |
| User tasks | 4/5/6 | LOW/NORMAL/FOREGROUND |

### Data structures

- `task_info_t` — per-FreeRTOS-task: pid, parent pid, type, argv, file_path, argc, errno, strtok saveptr, seed, children queue
- `task_thread_t` — shared among all threads within a process: PSRAM pages, dlmalloc state, 128 file handles, resource tracking (iconv, regex, open files, windows, devices, OTA, TLS)

### PIDs

- `NUM_PIDS` = 128, `MAX_PID` = 127; PID 0 reserved for kernel
- Circular PID allocator (ring buffer protected by `pid_table_lock`)

### Task types

- `TASK_TYPE_ELF` — loaded from in-memory buffer
- `TASK_TYPE_ELF_PATH` — loaded from filesystem path (read inside task context)
- `TASK_TYPE_THREAD` — shares parent's `task_thread_t` (same heap/files)

### FreeRTOS TLS usage

- Slot 0: reserved by ESP-IDF
- Slot 1: `task_info_t*` for BadgeVMS tasks; kernel uses static `kernel_task`
- Task application tag `0x12345678` distinguishes BadgeVMS tasks from kernel tasks

### Identifying current task context

```c
task_info_t *get_task_info()  // returns kernel_task if called from kernel context
```

### Exception handling

`cerberos()`: on unhandled exception in a user task, redirects `mepc` to a self-deleting stub (graceful kill without kernel crash). ESP panic handler (`__wrap_esp_panic_handler`) logs which PID caused the crash.

### Process creation flow

1. Caller sends `zeus_command_message_t` to `zeus_queue`
2. Zeus allocates PID, creates `task_info_t`, maps PSRAM pages, spawns FreeRTOS task pinned to core 1
3. Zeus notifies caller via `xTaskNotifyIndexed` with the new PID

### Resource tracking

Resources are tracked in `task_thread_t::resources[]` (khash maps). On process death, Hades closes/frees: file handles, iconv, regex, windows, devices, OTA sessions, TLS connections.

## Init Daemon (`badgevms/init.c`)

- Reads `FLASH0:init.toml` (embedded in firmware binary, always overwritten at boot) then optionally merges `SD0:init.toml`
- init.toml format (TOML):

```toml
[[apps]]
name = "myapp"
path = "APPS:[myapp]myapp.elf"
application = "myapp_uid"   # optional: links to application_t for dedup
restart_on_failure = true
run_once = false
stack_size = 16384
start_delay = 5             # seconds before first launch
start_every = 3600          # periodic restart interval (seconds)
args = ["--arg1"]
```

- Supervision loop runs every 1 second; throttles restarts to ≥2 s between attempts
- `run_once` apps tracked in NVS namespace `badgevms_init`
- Calls `validate_ota_partition()` after all initial apps start — marks firmware good

## Compositor (`badgevms/compositor/`)

Public API: `badgevms/include/badgevms/compositor.h`

- Up to 10 windows (`MAX_WINDOWS`)
- Display: 720×720 px, RGB565 (16bpp), 60 Hz refresh
- 3 display framebuffers (`DISPLAY_FRAMEBUFFERS`)
- Up to 10 pending events per window (`WINDOW_MAX_EVENTS`)
- Double-buffered windows available via `WINDOW_FLAG_DOUBLE_BUFFERED`

Window flags (bitmask):
```
FULLSCREEN, ALWAYS_ON_TOP, UNDECORATED, MAXIMIZED,
MAXIMIZED_LEFT, MAXIMIZED_RIGHT, DOUBLE_BUFFERED,
LOW_PRIORITY, FLIP_HORIZONTAL, FLIP_VERTICAL
```

Key functions:
- `window_create(title, size, flags)` → `window_handle_t`
- `window_framebuffer_get(window)` → `framebuffer_t*`
- `window_present(window, block, rects, num_rects)` — flip/blit to display
- `window_event_poll(window, block, timeout_ms)` → `event_t`
- `get_screen_info(w, h, format, refresh_rate)`

ESP32P4 PPA hardware quirk: **hard crash if `height > 32 && (height % 32) == 1`** — avoid such framebuffer dimensions.

## Device Subsystem (`badgevms/device.c`, `include/badgevms/device.h`)

Device registry: global map of name → `device_t*`.

Device vtable (all optional):
```c
_open, _close, _read, _write, _lseek, _destroy
```

Extended device types inherit from `device_t`:
- `filesystem_device_t`: adds stat/fstat/unlink/rename/mkdir/rmdir/opendir/readdir/closedir
- `lcd_device_t`: adds _draw, _getfb, _set_refresh_cb
- `orientation_device_t`: adds _get_orientation, _get_orientation_degrees
- `gas_device_t`: adds _get_pressure, _get_temperature, _get_gas_resistance, _get_humidity
- `i2c_bus_device_t`: adds _scan, _device_create
- `i2c_device_t`: adds _get_address

## Logical Names (`badgevms/logical_names.c`)

VMS-style path aliases. Supports search lists (comma-separated, resolved left to right):
```c
logical_name_set("APPS:", "SD0:[BADGEVMS.APPS], FLASH0:[BADGEVMS.APPS]", false);
logical_name_resolve("APPS:", 0)  // first entry
logical_name_resolve("APPS:", 1)  // second entry
```

## OTA (`badgevms/ota.c`, `include/badgevms/ota.h`)

- Single global OTA session (atomic_flag prevents concurrent sessions)
- `ota_session_open()` → selects next update partition, calls `esp_ota_begin`
- Resource-tracked: Hades calls `ota_session_abort()` if process dies mid-update
- `validate_ota_partition()` / `invalidate_ota_partition()` control boot validity
