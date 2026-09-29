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

The initial prompt backend is deterministic and intentionally narrow. It handles phrases such as `red`, `green`, `blue`, `orange`, `white`, `off`, and `set brightness to N`. The backend interface is replaceable by an on-device model or a remote model service.

### 5. On-device model track (current next phase)

The first real AI target is the ESP32-S3 itself. We will start with a tiny intent classifier, not a general chat model:

1. Define a fixed intent set (`led.set`, `led.off`, unsupported) and slot fields (`color`, `brightness`).
2. Create a small labeled prompt dataset and a host-side training/evaluation script.
3. Benchmark a compact quantized runtime on the N16R8, keeping the model and tensor arena in PSRAM where possible.
4. Add confidence thresholds: high-confidence commands execute, uncertain commands require confirmation, and unsupported prompts are refused.
5. Keep the deterministic parser as a fallback and test both backends against the same command validator.

The first model is expected to be a classifier/slot extractor rather than an unrestricted language model. The recommended ESP32-S3 prototype is a tiny hashed character n-gram classifier with int8 weights exported as a C header; it avoids a heavyweight tokenizer/runtime and handles small spelling variations. TensorFlow Lite Micro or ESP-DL can be evaluated later if the model grows. A real Needle-sized generative model is a later ESP32-P4/Luckfox target.

### 6. Model boundary

`ModelBackend` will support:

- No-model deterministic parsing on ESP32-S3.
- Tiny classifier/slot extractor on ESP32-S3.
- Larger ESP32-P4 backend.
- Luckfox/Linux C++ or Python backend.
- Full Needle-compatible gateway.

Every backend emits the same command envelope and remains subject to firmware validation.

### 6. Additional transports and devices

After serial stability: HTTP, WebSocket/MQTT, BLE, GPIO, sensors, relays, and displays. Transport and hardware implementations must not alter the command protocol.

## Initial acceptance tests

- `device.info` and `device.capabilities` return valid metadata.
- `led.set` accepts valid RGB/brightness values.
- Invalid ranges, unknown tools, malformed JSON, and oversized input are rejected.
- Plain prompts produce validated LED commands or a clear unsupported response.
- WS2812B status feedback distinguishes ready, processing, success, and error.
