# BadgeVMS Known Issues and Hardware Quirks

## Active Bugs

- **Events**: Only delivered once per frame, up to 10 per window. Should be more granular.
- **`select()`**: Included but only partially works.
- **LWIP heap corruption**: LWIP allocates in task context but frees in LWIP task — different dlmalloc heaps. Workaround: LWIP doesn't use SPIRAM.
- **Thread/process race**: Data race between thread/process creation and a process being killed while the creation message is in flight to Zeus → thread can leak.
- **Flash I/O**: Should be handled by a dedicated kernel task (currently runs in calling task).
- **WiFi sequencing**: Known sequencing problems in wifi connect/disconnect.
- **BMI270 IMU**: Only one axis is currently reported; if device is busy, stale results are returned (should wait until next cycle).
- **Compositor structure**: Needs restructuring to be easier to work with.
- **Scaled windows**: Scaled window content always appears at top — no pillarboxing or letterboxing.
- **Single-buffered windows**: Could be improved.
- **FreeRTOS sync primitives in tasks**: Tasks should never hold FreeRTOS mutex/semaphore across a yield point — if killed, FreeRTOS may kill a random task after timeout.
- **`WINDOW_FLAG_FLIP_HORIZONTAL`**: Meaning is hardcoded for WHY2025 badge, not generic.

## Hardware Quirks

- **ESP32P4 PPA hardware**: Hard crash if framebuffer/blit `height > 32 && (height % 32) == 1`. Avoid these dimensions.
- **PSRAM at 200 MHz**: Some devices get single-bit errors under heavy memory I/O. No workaround — PSRAM speed is capped lower in practice.
- **ESP-IDF FreeRTOS TLS slot 0**: ESP-IDF owns this slot and will overwrite it — BadgeVMS uses slot 1.
- **`CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS`**: If disabled, LWIP will leak memory when the task using it exits.

## ELF Loader Limitations

- BadgeVMS binaries must be position-independent ELF shared objects (`-fPIC -shared`). Other ELF types will not load.
- Only symbols named `main` should be exposed (`-fvisibility=hidden`).
- When linking `.a` libraries, use `-Wl,--exclude-libs,libname.a` to prevent accidentally exposing internal symbols.
- No `dlopen()` or shared libraries — all dependencies must be statically linked.

## Path System

- **VMS paths only** — Unix paths are not supported anywhere. Format: `DEVICE:[dir.subdir]file.ext`
- Apps frequently break if they accidentally use `/` or relative paths.
