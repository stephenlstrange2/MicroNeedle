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

Fetch the pinned TinyDecide engine and create the 6.2 MB flash image:

```sh
python3 tools/setup_tinydecide.py
```

The downloaded source and model are stored in ignored `.deps/` and `.models/` directories. The setup script pins the exact upstream revision used by the firmware.

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

## Optional Wi-Fi

If `MICRONEEDLE_WIFI_SSID` and `MICRONEEDLE_WIFI_PASSWORD` are defined in `build_flags`, firmware exposes:

```text
GET /health
GET /prompt?text=turn%20the%20LED%20orange
POST /prompt  {"prompt":"turn the LED orange"}
```

Serial remains available if Wi-Fi is not configured or cannot connect.

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
