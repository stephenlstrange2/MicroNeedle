# Changelog

All notable changes are documented here.

## Unreleased

### Added

- Repository-wide agent instructions requiring changelog entries and reproducible evidence for every change.
- A built-in, non-removable `status_led` device backed by the configured WS2812B pin.
- Task-executed RGB, brightness, on/off, state, blink, pulse, and rainbow LED operations.
- Deterministic explicit-color routing with an `unsupported_color` response for unknown colors.
- `led.pattern` JSON tool and RGB/pattern details in task completion results.

### Changed

- Onboard LED prompt, TinyDecide, fallback, and JSON paths now submit validated executor tasks instead of mutating the NeoPixel directly.
- Runtime status feedback no longer overwrites the user-selected LED state.
- Device listing and description include the system LED, its driver, capabilities, pin, and live state.
- PlatformIO serial monitoring enables local echo so typed commands remain visible.
