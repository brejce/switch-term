#!/usr/bin/env python3
"""Build the SIT-001 soundtrack: narration + print foley, no music."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import soundfile as sf

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
SR = 48000
DUR = 30.0

rng = np.random.default_rng(11)


def load(name: str) -> np.ndarray:
    x, sr = sf.read(HERE / "audio" / name, always_2d=False)
    assert sr == SR, (name, sr)
    if x.ndim > 1:
        x = x.mean(1)
    # trim near-silence at both ends
    env = np.abs(x)
    peak = env.max()
    idx = np.where(env > peak * 0.02)[0]
    if len(idx):
        x = x[idx[0]: idx[-1] + 1]
    return x.astype(np.float64)


def env_ad(n: int, attack: float, release: float) -> np.ndarray:
    a = max(1, int(attack * SR))
    r = max(1, n - a)
    return np.concatenate([np.linspace(0, 1, a), np.exp(-np.linspace(0, release, r))])


def band(x: np.ndarray, lo: float, hi: float) -> np.ndarray:
    spec = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    mask = np.ones_like(f)
    mask[f < lo] = 0
    mask[f > hi] = 0
    return np.fft.irfft(spec * mask, len(x))


def whoosh(dur=0.44) -> np.ndarray:
    n = int(dur * SR)
    x = rng.standard_normal(n)
    x = band(x, 400, 6000)
    e = np.linspace(0, 1, n) ** 2 * np.exp(-np.linspace(0, 3.2, n))
    return x * e / (np.abs(x).max() + 1e-9) * 0.5


def thump() -> np.ndarray:
    n = int(0.45 * SR)
    t = np.arange(n) / SR
    body = np.sin(2 * np.pi * 58 * t) * np.exp(-t * 11)
    body += 0.5 * np.sin(2 * np.pi * 96 * t) * np.exp(-t * 17)
    click = band(rng.standard_normal(n), 900, 5200) * np.exp(-t * 90) * 0.5
    x = body + click
    return x / (np.abs(x).max() + 1e-9) * 0.9


def rustle(dur=0.5) -> np.ndarray:
    n = int(dur * SR)
    x = band(rng.standard_normal(n), 1200, 9000)
    e = np.abs(np.convolve(rng.standard_normal(n), np.ones(400) / 400, "same"))
    e = e / (e.max() + 1e-9) ** 1.4
    return x * e / (np.abs(x).max() + 1e-9) * 0.35


def click() -> np.ndarray:
    n = int(0.05 * SR)
    t = np.arange(n) / SR
    x = band(rng.standard_normal(n), 1500, 7000) * np.exp(-t * 220)
    x += 0.4 * np.sin(2 * np.pi * 2200 * t) * np.exp(-t * 320)
    return x / (np.abs(x).max() + 1e-9) * 0.5


def reverb(x: np.ndarray, wet=0.12, tail=0.30) -> np.ndarray:
    n = int(tail * SR)
    ir = rng.standard_normal(n) * np.exp(-np.arange(n) / SR * 18)
    ir = band(ir, 200, 5000)
    ir /= np.abs(ir).max() + 1e-9
    wetx = np.convolve(x, ir)[: len(x)]
    return (1 - wet) * x + wet * wetx * 0.7


def place(track: np.ndarray, x: np.ndarray, at: float, gain: float) -> None:
    i = int(at * SR)
    j = min(len(track), i + len(x))
    if j <= i:
        return
    track[i:j] += x[: j - i] * gain


def main() -> None:
    OUT.mkdir(exist_ok=True)
    track = np.zeros(int(DUR * SR))

    lines = [
        ("n1.wav", 0.55, 1.00),
        ("n2.wav", 3.62, 0.95),
        ("n3.wav", 7.02, 0.95),
        ("n4.wav", 11.32, 0.95),
        ("n5.wav", 13.30, 0.92),
        ("n6.wav", 18.56, 0.95),
        ("n7.wav", 22.06, 1.05),
        ("n8.wav", 26.28, 0.95),
    ]
    for name, at, g in lines:
        x = load(name)
        # gentle low cut + presence for a dry, report-reading delivery
        x = x - band(x, 0, 90)
        x = reverb(x)
        place(track, x, at, g * 0.9)
        print(f"{name:8s} {len(x)/SR:5.2f}s @ {at:5.2f}s")

    # print foley
    for c in (3.20, 6.40, 11.00, 18.20, 21.60, 26.00):
        place(track, whoosh(), c - 0.22, 0.35)
        place(track, rustle(), c - 0.30, 0.30)
    for c in (22.60, 27.00):
        place(track, thump(), c, 0.85)
        place(track, rustle(0.3), c - 0.05, 0.25)
    for k in range(5):
        place(track, click(), 7.58 + 0.52 * k, 0.30)
    place(track, click(), 11.45, 0.35)
    place(track, click(), 12.9, 0.3)

    # fade the last 0.4s and the first 0.05s
    n_out = int(0.4 * SR)
    track[-n_out:] *= np.linspace(1, 0, n_out)
    track[: int(0.05 * SR)] *= np.linspace(0, 1, int(0.05 * SR))

    track /= np.abs(track).max() + 1e-9
    track *= 0.89
    sf.write(OUT / "audio.wav", track, SR)
    print("wrote", OUT / "audio.wav", f"{len(track)/SR:.2f}s")


if __name__ == "__main__":
    main()
