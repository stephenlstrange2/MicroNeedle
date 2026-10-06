# MicroNeedle Roadmap

MicroNeedle is an offline-first device-control runtime for microcontrollers. The first target is an ESP32-S3 DevKitC-1 N16R8 with a WS2812B, while the protocol and runtime boundaries remain portable to ESP32-P4, Luckfox Pico Plus, and Linux-capable processors.

## Core architecture

```text
User text prompt
      ↓
Serial / HTTP / WebSocket / BLE transport
      ↓
Prompt backend (rules first, model later)
      ↓
Validated versioned tool command
      ↓
Hardware abstraction layer
      ↓
WS2812B, GPIO, sensors, and future peripherals
```

Raw prompt/model output must never execute hardware directly. All actions pass through the command registry and schema validation.

## Milestones

### 1. ESP32-S3 baseline

- PlatformIO with pioarduino, Arduino, 16 MB flash, and 8 MB OPI PSRAM.
- Configurable WS2812B pin; default GPIO 48.
- Boot diagnostics and firmware/protocol version.
- LED status patterns for boot, ready, processing, success, error, and low memory.

### 2. Portable protocol

Versioned JSON request envelope:

```json
{"v":1,"id":"42","tool":"led.set","args":{"r":255,"g":20,"b":0,"brightness":80}}
```

Prompt envelope:

```json
{"v":1,"id":"43","prompt":"set the LED orange"}
```

Responses contain the request ID, success/error state, structured error codes, and optional interpreted tool/arguments.

### 3. Deterministic command engine

Initial tools:

- `device.info`
- `device.capabilities`
- `led.set`
- `led.off`
- `led.pattern`

The engine validates ranges, rejects unknown tools, limits input size, and returns structured errors. Serial JSON-lines is the first transport.

### 4. Prompt input

The first prompt interface is serial JSON-lines and plain-text serial prompts. An optional Wi-Fi HTTP interface is enabled at compile time with credentials and accepts `GET /prompt?text=...`; serial remains the fallback. A future JSON `POST /prompt` endpoint will share the same backend.

The primary prompt backend is now TinyDecide, a 10.4M-parameter 4-bit zero-shot decision model running locally on the ESP32-S3. It selects actions and colors and extracts brightness in a single encoder pass. The earlier n-gram classifier remains the recovery backend when the model partition is unavailable.

### 5. On-device model track (implemented, hardware validation next)

1. A dedicated flash partition holds TinyDecide's 6.2 MB model and vocabulary.
2. The ESP32-S3 PIE vector kernel uses both cores; inference scratch allocations prefer PSRAM.
3. The backend reports probability, decision confidence, latency, tokens, and truncation.
4. Model results are translated into the versioned tool contract and validated before execution.
5. Unsupported or ungrounded choices are rejected; runtime initialization failures use the small fallback model.

The next work is on-device accuracy/latency evaluation, threshold calibration, richer tool schemas, and correction prototypes. A generative Needle-style backend remains a later ESP32-P4/Luckfox target.

### 6. Model boundary

`ModelBackend` will support:

- No-model deterministic parsing on ESP32-S3.
- Tiny classifier/slot extractor on ESP32-S3.
- Larger ESP32-P4 backend.
- Luckfox/Linux C++ or Python backend.
- Full Needle-compatible gateway.

Every backend emits the same command envelope and remains subject to firmware validation.

### 7. Additional transports and devices

After serial stability: HTTP, WebSocket/MQTT, BLE, GPIO, sensors, relays, and displays. Transport and hardware implementations must not alter the command protocol.

## Initial acceptance tests

- `device.info` and `device.capabilities` return valid metadata.
- `led.set` accepts valid RGB/brightness values.
- Invalid ranges, unknown tools, malformed JSON, and oversized input are rejected.
- Plain prompts produce validated LED commands or a clear unsupported response.
- WS2812B status feedback distinguishes ready, processing, success, and error.
