#!/usr/bin/env python3
"""Fetch the pinned TinyDecide engine and build its ESP32 partition image."""
from __future__ import annotations

import hashlib
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REVISION = "5e0481b814f5158ae799b92d7427f211b7739c98"
REPOSITORY = "https://huggingface.co/TheREZOR/TinyDecide"
DEPENDENCY = ROOT / ".deps" / "TinyDecide"
MODEL_DIR = ROOT / ".models"
MODEL = MODEL_DIR / "model.bin"
IMAGE = MODEL_DIR / "tinydecide-esp32.bin"
VOCAB = DEPENDENCY / "esp32" / "tinydecide" / "model" / "vocab.bin"


def run(*args: str) -> None:
    print("+", " ".join(args))
    subprocess.run(args, check=True)


def fetch_engine() -> None:
    if not (DEPENDENCY / ".git").exists():
        DEPENDENCY.parent.mkdir(parents=True, exist_ok=True)
        # Hugging Face's Git server does not reliably support partial-clone
        # promisor fetches, so use a shallow full checkout.
        run("git", "clone", "--depth=1", REPOSITORY, str(DEPENDENCY))
    run("git", "-C", str(DEPENDENCY), "fetch", "--depth=1", "origin", REVISION)
    run("git", "-C", str(DEPENDENCY), "sparse-checkout", "init", "--cone")
    run("git", "-C", str(DEPENDENCY), "sparse-checkout", "set", "esp32/tinydecide")
    run("git", "-C", str(DEPENDENCY), "checkout", "--detach", REVISION)


def fetch_model() -> None:
    MODEL_DIR.mkdir(parents=True, exist_ok=True)
    url = f"{REPOSITORY}/resolve/{REVISION}/model.bin?download=true"
    if not MODEL.exists():
        print(f"Downloading {url}")
        urllib.request.urlretrieve(url, MODEL)
    if MODEL.stat().st_size < 6_000_000:
        raise RuntimeError(f"Downloaded model is unexpectedly small: {MODEL.stat().st_size} bytes")


def pack_image() -> None:
    if not VOCAB.exists():
        raise RuntimeError(f"TinyDecide vocabulary not found: {VOCAB}")
    image = MODEL.read_bytes() + VOCAB.read_bytes()
    IMAGE.write_bytes(image)
    digest = hashlib.sha256(image).hexdigest()
    print(f"Wrote {IMAGE} ({len(image):,} bytes, sha256 {digest})")
    if len(image) > 0x660000:
        raise RuntimeError("Packed image does not fit the 0x660000-byte tinydecide partition")


def main() -> int:
    try:
        fetch_engine()
        fetch_model()
        pack_image()
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print(f"setup failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
