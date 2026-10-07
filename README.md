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

Explicit LED colors are routed deterministically; less-direct language may use TinyDecide. Every LED mutation is submitted to the bounded task executor and produces a task completion event.

The onboard WS2812B is exposed as the built-in, non-removable `status_led` device. It supports on/off, RGB color, brightness, state, and non-blocking `blink`, `pulse`, and `rainbow` patterns:

```text
led orange
make the status light purple brightness 40
status led rainbow
turn off status led
```

```json
{"v":1,"id":"d1","tool":"device.describe","args":{"alias":"status_led"}}
{"v":1,"id":"p1","tool":"led.pattern","args":{"pattern":"pulse"}}
```

Unsupported explicit colors, such as `led brown`, return `unsupported_color` without model inference or hardware access.

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

JSON tools are declared in a typed registry. Before dispatch, the firmware rejects missing required fields, wrong types, values outside allowed ranges, invalid enum values, and unknown arguments. Prompt-derived tasks, confirmed configuration tasks, and direct JSON tasks then pass through the same semantic command validator for alias, capability, board-profile, resource-conflict, and system-device checks. `device.capabilities` returns the registered tools and their generated argument schemas/flags.

Task acknowledgements include routing provenance:

```json
{"routing":{"backend":"deterministic-device-router","mode":"deterministic","confidence":1.0}}
```

The N16R8 board profile conservatively rejects boot-strapping, USB, JTAG/debug, console, flash/PSRAM, and both known onboard WS2812B pins (GPIO 38/48). Never connect a mains-powered lamp directly to a GPIO; use a correctly rated and isolated relay or driver circuit.

Confirmation tokens are random, one-time, expire after 30 seconds, and occupy a bounded four-entry table. Tokens are bound to the originating serial or HTTP session and to a digest of the exact proposed operation. A request field such as `"confirmed": true` is intentionally ignored and cannot bypass confirmation.

## Optional Wi-Fi

Define `MICRONEEDLE_WIFI_SSID` and `MICRONEEDLE_WIFI_PASSWORD` in `build_flags` to enable:

```text
GET  /health
GET  /prompt?text=turn%20the%20LED%20orange
POST /request
GET  /events
```

`POST /request` accepts the same versioned JSON object used over serial, including prompts and direct tools. Request bodies are accumulated across HTTP chunks with a 768-byte limit. Both serial and HTTP feed an eight-entry bounded ingress queue; only the owner loop parses requests or mutates the registry, tasks, confirmations, NVS, and hardware.

TinyDecide runs on a dedicated serialized worker, so its multi-second inference does not stop task polling or the asynchronous `/health` endpoint. HTTP receives the real structured acknowledgement/error from the owner loop. Task terminal events are available as Server-Sent Events from `/events` and remain available through `task.status`/`task.list`.

For confirmation operations, supply a stable `X-MicroNeedle-Session` header on both proposal and confirmation requests. Without it, the remote IP is used as the session identity:

```sh
curl -H 'X-MicroNeedle-Session: client-1' \
  'http://DEVICE_IP/prompt?text=bind%20GPIO%204%20as%20desk%20lamp'

curl -H 'Content-Type: application/json' \
  -H 'X-MicroNeedle-Session: client-1' \
  --data '{"v":1,"id":"c1","tool":"confirmation.confirm","args":{"confirmation_id":123}}' \
  http://DEVICE_IP/request

curl -N http://DEVICE_IP/events
```

HTTP currently has session binding but no authentication or TLS. Keep it on a trusted network until Milestone H authorization is implemented.

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
