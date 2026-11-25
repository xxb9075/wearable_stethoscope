"""Realtime end-to-end simulation of the firmware stream (no hardware).

Emulates the device's ~150 ms bursts (20 MIC packets of 30 frames + 1 PPG) at a
true 4 kHz pace and checks that the writer reconstructs an accurate timeline:
the mean sample rate derived from the drift log must match 4 kHz, and the
arrival jitter must stay within bounds. This checks the host timeline only; it
does not measure timing against another instrument.
"""

from __future__ import annotations

import queue
import time

import numpy as np
import soundfile as sf

from capture import protocol
from capture.ringbuffer import RingBuffer
from capture.writer import STOP, Writer

from helpers import make_frames, mic_packet, ppg_packet

_BURST_S = 0.15
_FRAMES_PER_BURST = int(protocol.SAMPLE_RATE * _BURST_S)  # 600
_FRAMES_PER_PACKET = 30
_RUN_S = 5.0


def test_realtime_rate_and_timeline(tmp_path):
    q: "queue.Queue" = queue.Queue(maxsize=100000)
    rb = RingBuffer(capacity=protocol.SAMPLE_RATE * 12, n_channels=4)
    w = Writer(q, rb, tmp_path, sw_version="itest")
    w.start()

    all_frames = make_frames(int(protocol.SAMPLE_RATE * (_RUN_S + 1)))
    frame_i = 0
    start = time.perf_counter()
    n_bursts = int(_RUN_S / _BURST_S)

    for b in range(n_bursts):
        # Pace to real wall-clock so the measured rate reflects true timing.
        target = start + (b + 1) * _BURST_S
        now = time.perf_counter()
        if target > now:
            time.sleep(target - now)
        t_recv = time.perf_counter()
        burst = all_frames[frame_i : frame_i + _FRAMES_PER_BURST]
        frame_i += _FRAMES_PER_BURST
        raw = burst.tobytes()
        for p in range(0, len(raw), _FRAMES_PER_PACKET * protocol.BYTES_PER_FRAME):
            chunk = raw[p : p + _FRAMES_PER_PACKET * protocol.BYTES_PER_FRAME]
            q.put((mic_packet(chunk), t_recv))
        q.put((ppg_packet(), t_recv))

    q.put(STOP)
    w.join(timeout=15)
    assert not w.is_alive()

    import json

    meta = json.loads((tmp_path / "recording.json").read_text())
    total = meta["total_samples"]
    assert total == frame_i, (total, frame_i)

    # FLAC duration matches sample count.
    info = sf.info(str(tmp_path / "recording.flac"))
    assert info.frames == total

    # Measured mean rate should be within 2% of 4 kHz.
    mean_rate = meta["measured_mean_rate_hz"]
    assert mean_rate is not None
    assert abs(mean_rate - protocol.SAMPLE_RATE) / protocol.SAMPLE_RATE < 0.02, mean_rate

    # Per-interval jitter should be modest (bursty delivery, but bounded).
    assert meta["max_jitter_ms"] is not None
    assert meta["max_jitter_ms"] < 300, meta["max_jitter_ms"]

    # Bit-exact content.
    back, _ = sf.read(str(tmp_path / "recording.flac"), dtype="int16")
    assert np.array_equal(back, all_frames[:total])
