# MicroNeedle Roadmap

MicroNeedle is an offline-first device-control runtime for microcontrollers. The first target is an ESP32-S3 DevKitC-1 N16R8 with a WS2812B, while the protocol and runtime boundaries remain portable to ESP32-P4, Luckfox Pico Plus, and Linux-capable processors.

## Core architecture

```text
User text prompt
      ↓
Serial / HTTP / WebSocket / BLE transport
      ↓
TinyDecide model (n-gram recovery backend)
      ↓
Validated versioned tool command
      ↓
Named device registry
      ↓
Bounded priority task executor
      ↓
Hardware/remote-node drivers
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

## Named Device Registry and Task Executor

### Objective

Users address semantic devices rather than raw pins. A binding such as:

```text
assign GPIO 4 to a lamp called desk lamp
```

creates this resolution path:

```text
desk lamp → local ESP32-S3 → GPIO output → pin 4 → active-high → HIGH/LOW
```

Later bindings may target PWM, I²C/SPI/UART peripherals, sensors, remote ESP32 nodes, ESP32-P4, or Luckfox processors without changing the user-facing command format.

### 1. Board safety profile

Define the capabilities and restrictions of every board resource:

```cpp
struct PinCapability {
    uint8_t pin;
    bool inputAllowed;
    bool outputAllowed;
    bool pwmAllowed;
    bool reserved;
};
```

The N16R8 profile identifies safe GPIOs, flash/PSRAM pins, USB/JTAG pins, the WS2812B pin, restricted pins, and pins already claimed by drivers. Reserved pins and conflicting bindings must be rejected, and outputs must always enter a safe state during boot.

### 2. Device registry

Create a fixed-capacity semantic registry. The first target supports 16 devices, aliases up to 31 characters, unique normalized names, and future synonyms.

```cpp
struct DeviceBinding {
    uint32_t id;
    char alias[32];
    DeviceType type;
    DriverType driver;
    DeviceEndpoint endpoint;
    DeviceCapabilities capabilities;
    DeviceConfig config;
};
```

Example binding:

```json
{
  "alias": "desk_lamp",
  "display_name": "Desk Lamp",
  "driver": "gpio.output",
  "endpoint": {"node": "local", "pin": 4},
  "config": {"active_level": "high", "startup_state": "off"},
  "capabilities": ["on", "off", "state"]
}
```

Registry tools:

- `device.bind`
- `device.unbind`
- `device.rename`
- `device.list`
- `device.describe`
- `device.get`
- `device.set`

### 3. Persistent configuration

Store versioned bindings in ESP32 NVS. Persist identities and configuration, but not rapidly changing live state, to avoid flash wear. Boot must configure safe output states before loading bindings, validate records against the current board profile, ignore corrupt records safely, and support schema migration and factory reset.

### 4. Hardware driver interface

All local and remote device types implement a common interface:

```cpp
class DeviceDriver {
 public:
    virtual bool begin(const DeviceBinding&) = 0;
    virtual TaskResult execute(const DeviceCommand&) = 0;
    virtual DeviceState readState() = 0;
    virtual void safeStop() = 0;
};
```

Initial drivers:

1. `gpio.output`
2. `gpio.input`
3. `ws2812.rgb`
4. `gpio.pwm`

Later drivers include relays, buttons, ADC/I²C/SPI/UART devices, and remote-node proxies. Users address aliases such as `desk lamp`, `ventilation fan`, or `temperature sensor`; drivers translate semantic operations into hardware behavior.

### 5. Typed tool registry

Tool definitions remain separate from handlers:

```cpp
struct ToolDefinition {
    ToolId id;
    const char* name;
    const char* description;
    ToolSchema schema;
    ToolHandler handler;
    ToolFlags flags;
};
```

Every request follows:

```text
input → tool lookup → schema validation → safety/authorization validation
      → task creation → execution → structured result
```

### 6. Bounded priority task executor

Adopt the useful execution concepts from `edge-executor` without introducing Rust into the current Arduino firmware:

- Fixed task pool and bounded queues
- Explicit task handles
- Cooperative polling for long-running operations
- Pluggable execution lanes
- Controlled memory use
- Priority-aware scheduling

Task lifecycle:

```text
queued → running → completed | failed | cancelled | timed_out
```

Priorities:

1. `critical` — emergency stop and safe shutdown
2. `interactive` — user commands
3. `normal` — sensor and GPIO work
4. `background` — animations, logging, and maintenance

The initial executor has 16 task slots, bounded priority queues, and 16 retained results. Weighted scheduling prevents permanent starvation of normal or background work. Queue exhaustion returns `queue_full` instead of allocating indefinitely.

```cpp
struct Task {
    uint32_t id;
    ToolId tool;
    TaskPriority priority;
    TaskStatus status;
    uint32_t createdAt;
    uint32_t deadline;
    DeviceCommand command;
    TaskResult result;
};
```

Task tools:

- `task.status`
- `task.cancel`
- `task.list`

### 7. Execution lanes

Serialize resources that cannot operate concurrently:

- `inference` — one TinyDecide request at a time
- `gpio` — local pin changes
- `i2c` — one transaction per bus
- `network` — remote-node requests
- `effects` — animations and timed output operations

Long tasks expose a cooperative `poll()` step returning `continue`, `waiting`, `completed`, or `failed`. This permits serial, HTTP, and safety tasks to continue while animations and timed operations are active. TinyDecide remains a serialized blocking worker protected by its mutex.

### 8. Protocol extensions

Bind a device:

```json
{
  "v": 1,
  "id": "bind-1",
  "tool": "device.bind",
  "args": {
    "alias": "desk_lamp",
    "driver": "gpio.output",
    "pin": 4,
    "active_level": "high",
    "startup_state": "off"
  }
}
```

Use the alias:

```json
{
  "v": 1,
  "id": "set-1",
  "tool": "device.set",
  "args": {"alias": "desk_lamp", "state": "on"}
}
```

Task acceptance and completion are separate events:

```json
{"v":1,"id":"set-1","success":true,"task":{"id":27,"status":"queued"}}
```

```json
{
  "event": "task.completed",
  "task": {
    "id": 27,
    "tool": "device.set",
    "result": {"alias": "desk_lamp", "state": "on"}
  }
}
```

### 9. Privileged configuration and confirmation

Persistent hardware changes are more privileged than ordinary control. Model-generated `device.bind`, `device.unbind`, and endpoint changes produce a proposal and require explicit confirmation:

```json
{
  "success": false,
  "requires_confirmation": true,
  "confirmation_id": "cfg-83",
  "proposed": {
    "tool": "device.bind",
    "args": {"alias":"desk_lamp","driver":"gpio.output","pin":4}
  }
}
```

A trusted/admin transport may eventually execute direct JSON configuration under an explicit policy. The model may never bypass board-profile, conflict, capability, or range validation.

### 10. TinyDecide routing

TinyDecide determines, in one pass where practical:

1. Whether the request is configuration, control, query, or unsupported
2. Which registered device is referenced
3. Which supported operation is requested
4. The requested value, pin, alias, or other slot

Current aliases become dynamic choice options. TinyDecide may only select registered devices; an unknown or low-confidence target is refused or requires confirmation. When the registry exceeds the model's 32-option limit, a deterministic retrieval stage selects a smaller candidate set before inference.

### 11. Remote devices

Separate a device's semantic identity from its physical location:

```json
{
  "alias": "garage_light",
  "driver": "remote.device",
  "endpoint": {"node":"garage-esp32","device":"light"},
  "capabilities": ["on", "off", "state"]
}
```

The command `turn on garage light` remains unchanged. Candidate transports include UART, ESP-NOW, MQTT, HTTP, WebSocket, and BLE. Each remote node publishes capabilities and performs its own final safety validation.

### Implementation order

1. Board safety profile
2. In-memory device registry
3. GPIO output driver
4. `device.bind`, `device.list`, `device.get`, `device.set`, and `device.unbind`
5. NVS persistence and schema versioning
6. Fixed task pool and priority queues
7. Move the existing WS2812B behind the driver abstraction
8. Task status, cancellation, deadlines, and timeouts
9. TinyDecide alias/action routing
10. Configuration confirmation flow
11. PWM, digital input, and sensor drivers
12. Remote-node capability and transport support

### Current implementation status

Implemented in firmware v0.2.0:

- Conservative N16R8 pin safety profile
- 16-entry persistent named GPIO-output registry in NVS
- Active-high/active-low behavior and safe startup state
- `device.bind`, `unbind`, `rename`, `list`, `describe`, `get`, and `set`
- Four-entry random, one-time, expiring confirmation table
- 16-slot weighted-priority task executor with retained completion records
- Task status/list/cancel and structured terminal events
- TinyDecide alias/operation routing with deterministic explicit-alias grounding
- N-gram/deterministic recovery behavior

Serial is the supported configuration transport for this milestone. HTTP prompt handling remains experimental and disabled by default pending a bounded ingress queue and transport-specific response sink.

### First acceptance milestone

The first end-to-end milestone supports:

```text
bind GPIO 4 as desk lamp
list devices
turn on desk lamp
turn off desk lamp
get desk lamp state
rename desk lamp
remove desk lamp
```

It must provide persistent bindings, reserved-pin validation, safe boot state, structured task results, TinyDecide routing, n-gram recovery, and no direct unvalidated hardware access.
