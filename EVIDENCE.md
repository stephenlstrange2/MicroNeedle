# Verification Evidence

This file records reproducible validation evidence. A successful firmware build proves compilation and linkage only; hardware behavior remains pending until observed on a board.

## Unreleased

### Documentation policy

- Evidence: `AGENTS.md` exists at the repository root and requires updates to this file and `CHANGELOG.md` before proposing a commit.
- Status: implemented; enforcement is procedural.

### Milestone A — Built-in WS2812B system device

- Command: `$HOME/.venvs/platformio/bin/pio run -e esp32-s3-devkitc-1-n16r8`
- Result: passed on 2026-10-06; firmware linked and `firmware.bin` plus `firmware.factory.bin` were generated.
- Size evidence: 45,056 bytes RAM of 327,680 (13.8%); 537,908 bytes flash of 3,145,728 (17.1%).
- Supports: compilation/linkage of the `ws2812.rgb` driver, built-in registry binding, RGB/pattern task operations, deterministic LED prompt routing, and JSON tools.

- Command: `git diff --check`
- Result: passed on 2026-10-06 with no whitespace errors.

- Source inspection: hardware writes (`digitalWrite`, NeoPixel `setPixelColor`/`show`) are confined to `src/device_driver.cpp`; `src/main.cpp` has no direct LED hardware mutation calls.
- Source inspection: GPIO 38 and 48 remain reserved by `BoardProfile::isSystemClaimed`.

#### Hardware acceptance still pending

After uploading to an ESP32-S3 board, verify and record actual serial output and physical LED behavior for:

```text
list devices
led orange
led blue brightness 40
status led blink
status led pulse
status led rainbow
turn off status led
led brown
```

Also verify JSON `device.describe` for `status_led`, task IDs and completion events for every valid operation, `unsupported_color` for brown, preservation of existing NVS user bindings, and the actual onboard LED pin for the board revision. Until these checks are observed, Milestone A is source-complete and build-verified, not hardware-accepted.

### Milestone B — Typed tools and centralized validation

- Command: `$HOME/.venvs/platformio/bin/pio run -e esp32-s3-devkitc-1-n16r8`
- Result: passed on 2026-10-06 after adding `tool_registry.cpp` and `command_validator.cpp`.
- Size evidence: 45,056 bytes RAM of 327,680 (13.8%); 543,308 bytes flash of 3,145,728 (17.3%).
- Supports: compilation/linkage of typed tool definitions, generated capability schemas, centralized JSON validation, centralized task validation, typed-ID dispatch, and routing provenance.

- Command: `git diff --check`
- Result: passed on 2026-10-06 with no whitespace errors.

- Source inspection: `device.capabilities` iterates `ToolRegistry::count()` rather than maintaining a manual advertised-tool list.
- Source inspection: `handleTool` resolves a `ToolDefinition`, validates its argument object, and dispatches by `ToolId`; no `strcmp(tool, ...)` dispatch remains.
- Source inspection: the only call to `TaskExecutor::submit` is the shared `submitTask` path, and `TaskExecutor::submit` invokes `CommandValidator::validate` before allocating or queueing a task.
- Source inspection: confirmation proposals also invoke `CommandValidator::validate` before tokens are allocated.

#### Serial acceptance still pending

After upload, exercise representative schema failures and verify `invalid_args` without a task or hardware mutation:

```json
{"v":1,"id":"bad1","tool":"led.set","args":{"r":256,"g":0,"b":0}}
{"v":1,"id":"bad2","tool":"led.pattern","args":{"pattern":"sparkle"}}
{"v":1,"id":"bad3","tool":"device.set","args":{"alias":"desk_lamp","state":"maybe"}}
{"v":1,"id":"bad4","tool":"device.list","args":{"unexpected":true}}
{"v":1,"id":"bad5","tool":"device.bind","args":{"alias":"unsafe","pin":48}}
```

Verify valid direct JSON and equivalent prompts both queue tasks, deterministic prompts report `routing.mode: deterministic`, TinyDecide-assisted prompts report `routing.mode: model`, and `device.capabilities` returns schemas only for implemented tools. Until observed, Milestone B is source-complete and build-verified, not serial hardware-accepted.

### Milestone C — Safe ingress and transport responses

- Default command: `$HOME/.venvs/platformio/bin/pio run -e esp32-s3-devkitc-1-n16r8`
- Result: passed on 2026-10-06.
- Default size: 58,680 bytes RAM of 327,680 (17.9%); 557,788 bytes flash of 3,145,728 (17.7%).

- Wi-Fi compile command: `PLATFORMIO_BUILD_FLAGS='-DBOARD_HAS_PSRAM -mfix-esp32-psram-cache-issue -DCORE_DEBUG_LEVEL=3 -DKM_RGB_PIN=48 -DMICRONEEDLE_WIFI_SSID=\"test\" -DMICRONEEDLE_WIFI_PASSWORD=\"test\"' $HOME/.venvs/platformio/bin/pio run -e esp32-s3-devkitc-1-n16r8`
- Result: passed on 2026-10-06, including `/health`, `/prompt`, chunked `/request`, and `/events` code paths normally excluded without credentials.
- Wi-Fi size: 76,332 bytes RAM of 327,680 (23.3%); 1,063,499 bytes flash of 3,145,728 (33.8%).

- Command: `git diff --check`
- Result: passed on 2026-10-06 with no whitespace errors.

- Source inspection: HTTP callbacks call `enqueueHttp` and do not invoke prompt, tool, registry, task, NVS, model, or driver handlers.
- Source inspection: both serial and HTTP are consumed by `processIngress` on the owner loop through an eight-entry `IngressQueue`.
- Source inspection: all TinyDecide `interpret`/`routeDevice` calls are confined to `inference_worker.cpp`; results return to `processInferenceResults` before task/registry mutation.
- Source inspection: all normal protocol responses use `ResponseSink`; terminal task events are sent to serial and `/events` SSE clients.
- Source inspection: confirmation proposals store session plus an operation digest, and confirm/reject require the same session.
- Failure encountered and corrected: the first build used enum member `SERIAL`, which collided with Arduino's `SERIAL` macro. It was renamed to `SERIAL_INPUT`; both final builds pass.

#### Concurrent hardware/network acceptance still pending

With Wi-Fi credentials configured, upload and verify:

1. `/health` responds while a TinyDecide request is still running.
2. A deterministic serial command completes while HTTP TinyDecide inference is running.
3. Chunked `POST /request` returns the same structured task/confirmation/error objects as serial.
4. `/events` receives task terminal events, and `task.status` can retrieve the same task.
5. More than eight queued ingress requests return `ingress_queue_full` without corruption.
6. A confirmation proposed with session `client-1` fails under `client-2` with `confirmation_session_mismatch`, then succeeds once under `client-1`.
7. Oversized and disconnected HTTP bodies do not leak enough memory to impair subsequent requests.

Until these are observed, Milestone C is source-complete and build-verified, not concurrent hardware/network accepted.
