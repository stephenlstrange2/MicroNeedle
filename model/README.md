# ESP32-S3 Intent Model

This directory is reserved for the first real on-device model. The target is a compact intent classifier plus slot extraction, not a general conversational LLM.

## Initial labels

- `led.set`: color and optional brightness
- `led.off`
- `unsupported`

## Deployment contract

The model must emit a request equivalent to:

```json
{"v":1,"tool":"led.set","args":{"r":255,"g":80,"b":0,"brightness":80},"confidence":0.96}
```

The ESP32 command validator remains authoritative. Low-confidence or malformed model output is rejected.

## Development sequence

1. Expand `intents.jsonl` with real phrasing and negative examples.
2. Train/evaluate a compact classifier on a host machine.
3. Export a quantized artifact supported by the selected embedded runtime.
4. Measure flash, PSRAM, latency, and accuracy on the N16R8.
5. Enable it behind a build flag while retaining the deterministic fallback.
