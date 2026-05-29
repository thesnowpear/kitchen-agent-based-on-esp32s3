# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

> # ⚠️ 必读 / MUST READ BEFORE ANY TASK ⚠️
>
> **在动手做任何任务之前，必须先完整阅读仓库根目录的 `AGENTS.md`。**
>
> `AGENTS.md` 与本文件 **同等权威 (equal authority)**，并不是"补充材料"。两份文件构成一个整体：
>
> - `CLAUDE.md` 偏向"怎么构建、怎么烧录、代码在哪里、架构是什么"。
> - `AGENTS.md` 偏向"项目硬约束、硬件安全红线、备份工作流、中文注释规范、进度记录规则、踩坑经验"。
>
> 当两份文件出现冲突时：
> 1. 涉及 **硬件安全 / 备份流程 / 中文注释 / 进度记录格式** 的条款，以 `AGENTS.md` 为准。
> 2. 涉及 **构建/烧录命令、组件清单、协议字段、目录结构** 的条款，以 `CLAUDE.md` 为准。
> 3. 仍有歧义时，停下来问用户，不要凭推断动工。
>
> 同样必读的还有 `doc/memory.md`，它记录了已经验证过的接线、踩过的坑、不能再犯的错误。
>
> **跳过这些文件直接开工 = 极大概率违反项目规则、损坏硬件、或重复已经踩过的坑。**

---

## Project overview

"冰箱小精灵" (Fridge Spirit) is an ESP-IDF firmware project for an **ESP32-S3-DevKitC-1 N8R8** (8 MB Flash + 8 MB PSRAM) smart fridge-magnet device. The firmware drives a 720x720 QSPI screen (TR230S + FT6336U touch), OV3660 camera, MPU6050 IMU, BH1750/photoresistor, 24 GHz HLK presence radar, INMP441 mic, MAX98357A speaker, calls **OpenAI-compatible HTTPS APIs** directly from the device for chat / TTS / ASR (SiliconFlow is the current default backend), and exposes a USB JSON Lines console for a `web/` React+Vite operator panel.

This is a **hardware project** — GPIO, power, clock, and bus decisions can damage the board, screen, camera, or sensors. Review the hardware-safety section of `AGENTS.md` before touching any pin assignment, sdkconfig, or driver init.

## Build, flash, and monitor (ESP-IDF 6.0.1 on Windows)

The ESP-IDF install lives at `D:\esp-IDF\.espressif\v6.0.1\esp-idf`, but `export.ps1` defaults to `C:\Users\123\.espressif` and fails unless `IDF_TOOLS_PATH` is set first. Always use this exact sequence in PowerShell:

```powershell
$env:IDF_TOOLS_PATH='D:\esp-IDF\.espressif'
. D:\esp-IDF\.espressif\v6.0.1\esp-idf\export.ps1
idf.py build
idf.py -p COM16 flash
idf.py -p COM16 monitor
```

- Target chip: `esp32s3`. The board is at **COM16**.
- COM16 may be held by `idf_monitor`, VS Code Serial Monitor, Web Serial in the browser, or another terminal. If `Access to the port 'COM16' is denied` appears, close the holding process — do **not** retry-flash in a loop. When killing leftover monitor processes, only kill those whose command line contains `COM16` and `idf_monitor`/`idf.py monitor`.
- After toggling Kconfig options (notably `CONFIG_FRIDGE_SCREEN_TEST` / `CONFIG_FRIDGE_CAMERA_TEST`), run `idf.py reconfigure build` and check `build/config/sdkconfig.h` before reflashing.
- The main image must fit the large `ota_0` slot, while the small recovery image must fit `ota_1` (see `partitions_recovery.csv`). Use `idf.py size` / `idf.py size-components` to debug overflow.
- Recovery builds must be configured with `-DFRIDGE_RECOVERY_BUILD=ON -DSDKCONFIG="sdkconfig.recovery" -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.recovery.defaults"`. Do not use the default `idf.py flash` output from the recovery build directory as-is: ESP-IDF flashes the app image to `ota_0` by default, while the recovery image belongs at the `ota_1` offset from `partitions_recovery.csv`.

### Web operator panel (`web/`)

```bash
cd web
npm install
npm run dev      # vite dev server (host 0.0.0.0)
npm run build    # tsc -b && vite build  — REQUIRED before claiming a Web change works
npm run preview
```

`start_web_panel.bat` from the repo root will install deps, pick the first free port in `5173-5199`, start `npm run dev`, and open the browser. The panel talks to the firmware over **Web Serial** (Chrome/Edge only) using the JSON Lines protocol described below.

There is no test runner configured for either firmware or web — validation is real-board: build, flash, send USB JSON commands, watch the monitor log.

## Architecture

### Build entry & components

- Root `CMakeLists.txt` adds `example/ov3660_reference/esp32-camera` via `EXTRA_COMPONENT_DIRS`. Everything else lives under `components/`. Do not put feature code in `example/` — that directory is reference material pulled from upstream projects.
- `main/main.c` is **only** startup orchestration. It picks one of three modes from Kconfig and wires component init in a fixed order:
  - `CONFIG_FRIDGE_SCREEN_TEST=y` — standalone QSPI screen test task, **no** NVS / Wi-Fi / USB protocol. If this stays on by accident, Web Serial appears to time out on every command (see `doc/memory.md` 2026-05-23 entry).
  - `CONFIG_FRIDGE_CAMERA_TEST=y` — camera-only USB console; skips screen/sensors/audio. Mutually exclusive with screen test (enforced by `#error`).
  - `CONFIG_FRIDGE_RECOVERY_APP=y` — small local recovery shell for `ota_1`; starts NVS, diagnostics, Wi-Fi, and JSON Lines commands (`get_status`, `set_network`, `restore_main`, `boot_main`) only.
  - Default — full stack: `nvs → diagnostics → network → ai_client → mqtt_protocol → asr → sensors → radar → audio → speaker → wake_word → camera → usb_protocol`.
- Each subsystem under `components/` owns its own `CMakeLists.txt`, `include/`, NVS namespace, and FreeRTOS tasks. Cross-component access goes through the published `include/` header, never via internal statics. Hardware-touching components must document GPIO / voltage / clock / DMA / PSRAM constraints in their headers.
- Component map (kept in sync with `main/CMakeLists.txt` `REQUIRES`):
  - `network` — Wi-Fi STA, SNTP, NVS-backed credentials.
  - `ai_client` — HTTPS chat to OpenAI-compatible APIs; UTF-8-cleans every system/context/history/user string before serialization (see "AI HTTP 400 root cause" below).
  - `ai_context` — assembles project context + structured memory + recent dialog history before each AI call.
  - `storage` — NVS, LittleFS (`joltwallet/littlefs`), inventory snapshots, offline event queue.
  - `sensors` — BH1750/photoresistor on `GPIO1` (ADC1_CH0, **reverse polarity** — see memory.md), MPU6050 IMU on I2C. Always go through `fridge_sensors_get_snapshot()` — do not read ADC directly from USB or business code.
  - `radar` — HLK 24 GHz UART presence radar; both `Range xx` text frames and binary report frames appear, and the trailer is **not** a single fixed value (`08070605` and `F8F7F6F5` both seen).
  - `audio` — INMP441 I2S STD driver, 16 kHz / mono / left slot. Raw 32-bit slots are right-shifted to 16-bit PCM (default shift `>> 14`). Multipart bodies for ASR must be allocated in PSRAM.
  - `asr` — SiliconFlow `TeleAI/TeleSpeechASR` by default; NVS namespace `fridge_asr`, **separate** from AI config.
  - `wake_word` — ESP-SR WakeNet, model `wn9_xiaobinxiaobin` only (see `sdkconfig.defaults`); model image lives in the `model` partition (subtype `0x40`).
  - `speaker` — MAX98357A I2S TTS playback. SiliconFlow TTS model is `fnlp/MOSS-TTSD-v0.5`; **do not** use voice `alloy` (see `components/speaker/README.md`).
  - `camera` — OV3660 via the upstream `esp32-camera` component under `example/ov3660_reference/`.
  - `display_test`, `diagnostics`, `mqtt_protocol`, `usb_protocol` — self-explanatory; `usb_protocol` is the JSON Lines console below.

### USB JSON Lines protocol (firmware ↔ web/CLI)

- 115200 baud, one JSON object per line. Logs and responses share the same TX, so debug scripts must read line-by-line and only match `{...}` lines that contain the expected `request_id`.
- AI / ASR / Wi-Fi commands (`ai_assistant_chat`, `test_ai_chat`, `scan_wifi`, `set_network`, `voice_chat_stop`, etc.) can run 10–60 s. Use **90–130 s** timeouts; do not apply a generic short timeout.
- API keys must never appear in logs or responses. Log only HTTP status, response length, latency, task type, and history count.
- `WebSerialTransport` (in `web/src/transports/`) must resolve `response → pending` **before** dispatching to UI handlers, with try/catch around dispatch. Reversing this order causes spurious `get_status` timeouts (see memory.md 2026-05-23).

### Flash layout & sdkconfig

- `sdkconfig.defaults` is the source of truth for the N8R8 target. `partitions_recovery.csv` defines: `nvs(64K) | otadata | phy_init | ota_0(3584K main) | ota_1(1024K recovery) | assets(0x83, 1536K) | cache(0x83, 704K) | model(0x40, 768K) | coredump(64K)`. PSRAM is **octal** at 80 MHz, brownout and flash coredump are on.
- Custom `0x83` subtypes are project-defined data partitions (LittleFS / cache). `0x40` is the ESP-SR model image.
- `mbedtls` uses the bundled certificate store (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y`); do **not** hard-code per-vendor certs in firmware.

## Project rules you must follow

These come from `AGENTS.md` and `doc/memory.md` and override any general defaults.

1. **Hardware safety first.** Screen VCC is 5 V but logic is 3.3 V — never feed 5 V into a GPIO. Avoid strapping/Flash/PSRAM pins (GPIO 0, 35–37, 45, 46). Do not bump SPI/QSPI clocks, OV3660 XCLK, backlight duty, or GPIO drive strength "just to try". Stop and flag risk if wiring/power is unclear.
2. **Chinese comments are mandatory.** File headers, init functions, task functions, state-machine branches, and every hardware-driver step must have concise Chinese comments explaining what the block does and what hardware/timing constraints apply. Public interfaces, message structs, and state enums document field meanings in Chinese.
3. **Component-first.** New features go under `components/<name>/` with their own `CMakeLists.txt` and `include/`. `main/main.c` only orchestrates startup. Never let UI/network/storage/state code reach into another component's internals.
4. **Backups before edits.** Any in-repo edit needs a prior backup under `tmp/backups/{manual,before_run,archive}/YYYYMMDD-HHMMSS_短事项名/`, preserving the original relative path and filename. Do not scatter `.bak-*` next to source files, do not pile backups directly under `tmp/`. `tmp/logs/`, `tmp/notes/`, `tmp/cache/` are the only other approved categories. Do **not** delete historical backups without user confirmation.
5. **UTF-8 only.** All Chinese docs / comments / backups stay UTF-8. `doc/progress.md` has been observed with non-UTF-8 bytes — do not blind-append; verify encoding first or ask the user.
6. **Progress log.** Milestone-sized completions get a new `----`-separated block in `doc/progress.md` with a date, what changed, modules touched, current result, and risks/next steps. Small fixes attach as bullets under the most recent relevant block, not as a new block.
7. **AI request hygiene.** Every system/context/history/user text crossing the network must be UTF-8 validated/cleaned — a half-truncated multibyte character produced past `400 invalid unicode code point` errors. Empty/very-short ASR transcripts must be rejected before calling the AI to avoid 400s on empty `messages[0].content`. The voice chat path reuses the text chat history-trimming logic (`convert_storage_history_to_ai_history()`) — keep them in lockstep.
8. **Radar is context, not a door sensor.** "Presence=true" only means the module reported a target. Door state is derived from light delta + IMU.
9. **AI output is never auto-applied to inventory.** It must go through rule validation and explicit user confirmation; low-confidence results enter a pending queue.

## Subagents and skills

`AGENTS.md` authorizes automatic use of the `esp32-firmware-engineer` skill for ESP32 / ESP-IDF / FreeRTOS / peripheral driver / partition table / OTA / serial log / crash triage / LVGL / build-flash-monitor work, and authorizes spawning subagents to parallelize. Use them where they help.
