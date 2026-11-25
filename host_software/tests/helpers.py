"""Shared helpers for building firmware-accurate synthetic packets."""

from __future__ import annotations

import numpy as np

from capture import protocol


def make_frames(n_frames: int, seed: int = 0) -> np.ndarray:
    """Deterministic int16 frames where value(f, c) encodes (f, c)."""
    frames = np.empty((n_frames, protocol.N_CHANNELS), dtype="<i2")
    base = np.arange(n_frames)
    for c in range(protocol.N_CHANNELS):
        frames[:, c] = ((base * 7 + c * 1000 + seed) % 20000 - 10000).astype("<i2")
    return frames


def mic_packet(data_bytes: bytes) -> bytes:
    """Wrap payload bytes in a MIC modality header."""
    return protocol.HEADER_MIC.to_bytes(protocol.HEADER_LEN, "little") + data_bytes


def ppg_packet() -> bytes:
    """A realistic 10-byte PPG notification (should be ignored by the host)."""
    return protocol.HEADER_PPG.to_bytes(protocol.HEADER_LEN, "little") + bytes(
        [70, 5, 98, 2, 0, 100]
    )


def chop_into_packets(raw: bytes, sizes: list[int]) -> list[bytes]:
    """Chop a raw interleaved byte stream into MIC packets of the given sizes.

    Sizes deliberately need not be multiples of the frame size, to exercise the
    writer's carry buffer for frames straddling packet boundaries.
    """
    packets = []
    i = 0
    si = 0
    while i < len(raw):
        s = sizes[si % len(sizes)]
        packets.append(mic_packet(raw[i : i + s]))
        i += s
        si += 1
    return packets
