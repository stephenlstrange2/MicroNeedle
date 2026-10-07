# Changelog

All notable changes are documented here.

## Unreleased

Firmware metadata now reports `microneedle-0.3.0` for the system-device and typed-tool milestones.

### Added

- Repository-wide agent instructions requiring changelog entries and reproducible evidence for every change.
- A built-in, non-removable `status_led` device backed by the configured WS2812B pin.
- Task-executed RGB, brightness, on/off, state, blink, pulse, and rainbow LED operations.
- Deterministic explicit-color routing with an `unsupported_color` response for unknown colors.
- `led.pattern` JSON tool and RGB/pattern details in task completion results.
- Declarative typed tool registry with required-field, type, enum, range, and unknown-argument validation.
- Central command validator shared by prompt-derived, confirmed, and direct JSON task submissions.

### Changed

- Onboard LED prompt, TinyDecide, fallback, and JSON paths now submit validated executor tasks instead of mutating the NeoPixel directly.
- Runtime status feedback no longer overwrites the user-selected LED state.
- Device listing and description include the system LED, its driver, capabilities, pin, and live state.
- PlatformIO serial monitoring enables local echo so typed commands remain visible.
- Capability advertisement is generated from registered tool definitions instead of a duplicated manual list.
- Task acknowledgements use consistent `routing.backend`, `routing.mode`, and `routing.confidence` metadata; deterministic alias commands are no longer mislabeled as TinyDecide decisions.
- Registered JSON tools dispatch by stable typed IDs after centralized schema validation.
