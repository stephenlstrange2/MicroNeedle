# MicroNeedle

Offline-first prompt-to-device control for microcontrollers.

The first target is an ESP32-S3 DevKitC-1 N16R8 with a WS2812B. See [`ROADMAP.md`](ROADMAP.md) for the portable ESP32-P4/Luckfox architecture.

## Current prototype

MicroNeedle now uses [TinyDecide](https://huggingface.co/TheREZOR/TinyDecide), a 10.4M-parameter 4-bit decision model, directly on the ESP32-S3. It makes zero-shot action/color decisions and extracts brightness. Every result still passes through the firmware validator before hardware execution.

If the TinyDecide model partition is missing or initialization fails, firmware falls back to the earlier small n-gram classifier.

The firmware accepts plain prompts or newline-delimited JSON over USB serial:

```text
turn the LED purple at brightness 40
```

```json
{"v":1,"id":"2","prompt":"set the LED orange"}
```

Direct validated tools remain available:

```json
{"v":1,"id":"1","tool":"led.set","args":{"r":255,"g":80,"b":0,"brightness":80}}
```

A TinyDecide response reports its decision, confidence, extracted arguments, latency, token count, and truncation state.

## One-time model setup

PlatformIO Core **6.2.0 or newer** is required. If Fedora's DNF package is older, use the isolated environment described below:

```sh
python3 -m venv ~/.venvs/platformio
source ~/.venvs/platformio/bin/activate
python -m pip install --upgrade pip platformio
```

Fetch the pinned TinyDecide engine, the real Git-LFS model and vocabulary assets, and create the 6.4 MB flash image:

```sh
python3 tools/setup_tinydecide.py
```

The downloaded source and model are stored in ignored `.deps/` and `.models/` directories. The setup script pins the exact upstream revision used by the firmware and validates that `vocab.bin` starts with `TDV1`; this prevents accidentally packing the repository's Git-LFS pointer file.

## Build and flash

Build and upload the application and custom 16 MB partition table:

```sh
pio run -e esp32-s3-devkitc-1-n16r8
pio run -e esp32-s3-devkitc-1-n16r8 -t upload
```

Then write the TinyDecide model into its dedicated partition once:

```sh
python3 tools/flash_tinydecide.py --port /dev/ttyACM0
```

The model begins at `0x310000`, matching [`partitions.csv`](partitions.csv). Normal firmware uploads do not rewrite it. Flash it again after a full-chip erase or if the model revision changes.

Open the monitor:

```sh
pio device monitor -b 115200
```

At boot, look for:

```text
Model backend: TinyDecide 10.4M Q4
```

If it reports `ngram fallback`, verify the model image was flashed at the correct offset.

The RGB pin defaults to GPIO 48. Override `KM_RGB_PIN` in [`platformio.ini`](platformio.ini) for boards using GPIO 38 or another pin.

## Named devices and tasks

MicroNeedle can persistently bind a safe GPIO output to a semantic alias. Configuration changes require confirmation:

```text
bind GPIO 4 as desk lamp
```

The response contains a confirmation ID and an exact `confirmation_prompt`:

```json
{"success":false,"requires_confirmation":true,"confirmation_id":3438707108,"confirmation_prompt":"confirm 3438707108","expires_in_seconds":30,"proposed":{"tool":"device.bind"}}
```

You must send that second command within 30 seconds; the proposal alone does not modify hardware:

```text
confirm 3438707108
```

The binding is queued, executed, saved to NVS, and reported through a completion event. It survives reboot. You can then use natural language:

```text
turn on desk lamp
get desk lamp state
turn off desk lamp
list devices
rename desk lamp to work light
remove work light
```

Rename, removal, and binding operations require confirmation. TinyDecide routes control/query prompts to aliases; a deterministic device router is used if TinyDecide is unavailable.

Equivalent JSON tools include:

```json
{"v":1,"id":"b1","tool":"device.bind","args":{"alias":"desk lamp","pin":4}}
{"v":1,"id":"c1","tool":"confirmation.confirm","args":{"confirmation_id":1}}
{"v":1,"id":"s1","tool":"device.set","args":{"alias":"desk lamp","state":"on"}}
{"v":1,"id":"g1","tool":"device.get","args":{"alias":"desk lamp"}}
{"v":1,"id":"l1","tool":"device.list","args":{}}
```

All device operations run through a 16-slot bounded priority executor. Requests first receive a queued task ID and later produce `task.completed`, `task.failed`, `task.cancelled`, or `task.timed_out` events. Inspect and cancel tasks with `task.status`, `task.list`, and `task.cancel`.

The N16R8 board profile conservatively rejects boot-strapping, USB, JTAG/debug, console, flash/PSRAM, and both known onboard WS2812B pins (GPIO 38/48). Never connect a mains-powered lamp directly to a GPIO; use a correctly rated and isolated relay or driver circuit.

Confirmation tokens are random, one-time, expire after 30 seconds, and occupy a bounded four-entry table. A request field such as `"confirmed": true` is intentionally ignored and cannot bypass confirmation.

## Optional Wi-Fi

The HTTP prompt path is currently experimental and disabled by default while it is moved behind a bounded single-owner ingress queue. To compile the existing prototype, define `MICRONEEDLE_WIFI_SSID`, `MICRONEEDLE_WIFI_PASSWORD`, and `MICRONEEDLE_EXPERIMENTAL_HTTP_PROMPT` in `build_flags`. It exposes:

```text
GET /health
GET /prompt?text=turn%20the%20LED%20orange
POST /prompt  {"prompt":"turn the LED orange"}
```

Serial is the supported control transport for named-device configuration in this milestone. The experimental HTTP handlers return only generic HTTP results and may not receive full structured task/confirmation responses; do not expose them to untrusted networks.

## Flash layout

The N16R8's 16 MB flash is divided into:

- 3 MB application partition
- 6.375 MB TinyDecide data partition
- 6.5 MB storage/future expansion partition

TinyDecide uses PSRAM for inference scratch memory when available.

## Fallback model

The previous hashed n-gram classifier remains available for recovery. Its source dataset is [`model/intents.jsonl`](model/intents.jsonl). Retrain it with:

```sh
python3 model/train_ngram.py model/intents.jsonl --out model/intent_model.h
```

## Safety boundary

Model output never controls hardware directly. MicroNeedle maps decisions into allowlisted commands, checks argument ranges, and only then updates the WS2812B. Unsupported, ungrounded, invalid, or low-confidence results are rejected.
