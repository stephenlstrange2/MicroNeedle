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
