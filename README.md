# MicroNeedle

Offline-first prompt-to-device control for microcontrollers.

The first target is an ESP32-S3 DevKitC-1 N16R8 with a WS2812B. See [`ROADMAP.md`](ROADMAP.md) for the architecture and planned ESP32-P4/Luckfox extensions.

## Current prototype

The firmware accepts newline-delimited JSON over USB serial. It supports direct tools and text prompts:

```json
{"v":1,"id":"1","tool":"led.set","args":{"r":255,"g":80,"b":0,"brightness":80}}
```

```json
{"v":1,"id":"2","prompt":"set the LED orange"}
```

It also accepts a plain text line such as `turn the LED blue`.

If `MICRONEEDLE_WIFI_SSID` and `MICRONEEDLE_WIFI_PASSWORD` are defined in `build_flags`, it starts a Wi-Fi HTTP interface:

```text
GET /health
GET /prompt?text=turn%20the%20LED%20orange
```

Serial remains available if Wi-Fi is not configured or cannot connect.

## Build

Requires PlatformIO Core **6.2.0 or newer**. The current pioarduino stable platform rejects Core 6.1.19.

Upgrade the CLI if needed:

```sh
pio upgrade
# or, in the active Python environment:
python -m pip install --upgrade platformio
```

Then build:

```sh
pio run -e esp32-s3-devkitc-1-n16r8
pio run -e esp32-s3-devkitc-1-n16r8 -t upload
pio device monitor -b 115200
```

The RGB pin defaults to GPIO 48. Override `KM_RGB_PIN` in `platformio.ini` for boards using GPIO 38 or another pin.

## Model status

The firmware now includes a first tiny hashed character n-gram intent classifier generated from `model/intents.jsonl`. It classifies `led.set`, `led.off`, and `unsupported`; color/brightness slot extraction remains deterministic. Retrain with `python3 model/train_ngram.py model/intents.jsonl --out model/intent_model.h`. The replaceable backend interface is in `include/model_backend.h`.

## Safety boundary

Prompts are interpreted into allowlisted commands, then validated before hardware execution. Any future model backend must emit the same command shape and cannot bypass validation.
