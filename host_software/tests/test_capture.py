"""Unit tests for the capture backend (no hardware)."""

from __future__ import annotations

import json
import queue
import time

import numpy as np
import pytest
import soundfile as sf

from capture import protocol
from capture.ble import BleWorker
from capture.ringbuffer import RingBuffer
from capture.writer import STOP, Writer

from helpers import chop_into_packets, make_frames, ppg_packet


# --- protocol --------------------------------------------------------------

def test_parse_header():
    assert protocol.parse_header(ppg_packet()) == protocol.HEADER_PPG
    mic = protocol.HEADER_MIC.to_bytes(4, "little") + b"\x00\x00"
    assert protocol.parse_header(mic) == protocol.HEADER_MIC


def test_geometry_constants():
    assert protocol.BYTES_PER_FRAME == protocol.N_CHANNELS * 2 == 8
    assert protocol.SAMPLE_RATE == 4000
    assert len(protocol.CHANNEL_LABELS) == protocol.N_CHANNELS


# --- ring buffer -----------------------------------------------------------

def test_ringbuffer_basic_and_wrap():
    rb = RingBuffer(capacity=10, n_channels=4)
    a = make_frames(6)
    rb.append(a)
    assert np.array_equal(rb.read_last(4), a[-4:])
    b = make_frames(8, seed=1)
    rb.append(b)  # 14 total -> last 10 retained
    assert np.array_equal(rb.read_last(10), np.vstack([a, b])[-10:])
    assert rb.total_written == 14


def test_ringbuffer_big_append():
    rb = RingBuffer(capacity=10, n_channels=4)
    c = make_frames(25)
    rb.append(c)
    assert np.array_equal(rb.read_last(10), c[-10:])
    assert rb.total_written == 25


def test_ringbuffer_partial_read_when_underfilled():
    rb = RingBuffer(capacity=100, n_channels=4)
    a = make_frames(5)
    rb.append(a)
    out = rb.read_last(50)
    assert out.shape == (5, 4)


# --- writer ----------------------------------------------------------------

def _run_writer(tmp_path, packets, extra=None, gap_after=None):
    q: "queue.Queue" = queue.Queue(maxsize=100000)
    rb = RingBuffer(capacity=protocol.SAMPLE_RATE * 12, n_channels=4)
    w = Writer(q, rb, tmp_path, sw_version="test")
    w.start()
    t = time.perf_counter()
    for i, p in enumerate(packets):
        q.put((p, t))
        t += 1 / protocol.SAMPLE_RATE * 30  # ~ one 30-frame packet of stream time
        if gap_after is not None and i == gap_after:
            w.mark_gap(approx_lost_ms=123.0)
    if extra:
        for p in extra:
            q.put((p, t))
            t += 0.001
    q.put(STOP)
    w.join(timeout=15)
    assert not w.is_alive()
    return rb


def test_writer_bit_exact_roundtrip_with_split_frames(tmp_path):
    frames = make_frames(4096)
    raw = frames.tobytes()
    # sizes not all multiples of 8 -> exercises carry buffer
    packets = chop_into_packets(raw, sizes=[240, 13, 7, 100, 8, 3, 240, 5])
    rb = _run_writer(tmp_path, packets, extra=[ppg_packet()])

    back, sr = sf.read(str(tmp_path / "recording.flac"), dtype="int16")
    assert sr == protocol.SAMPLE_RATE
    assert back.shape == (4096, 4)
    assert np.array_equal(back, frames)
    assert np.array_equal(rb.read_last(4096), frames)


def test_writer_metadata_and_gap(tmp_path):
    frames = make_frames(2000)
    packets = chop_into_packets(frames.tobytes(), sizes=[240])
    _run_writer(tmp_path, packets, extra=[ppg_packet(), ppg_packet()], gap_after=5)

    meta = json.loads((tmp_path / "recording.json").read_text())
    assert meta["total_samples"] == 2000
    assert meta["n_channels"] == 4
    assert meta["sample_rate"] == 4000
    assert meta["non_mic_packets_ignored"] == 2
    assert meta["start_time_utc"] is not None
    assert meta["start_time_unix"] is not None
    # start time should be within ~1 min of now
    assert abs(meta["start_time_unix"] - time.time()) < 60
    assert len(meta["gaps"]) == 1
    assert meta["gaps"][0]["approx_lost_ms"] == 123.0
    assert 0 <= meta["gaps"][0]["sample_index"] <= 2000

    # drift csv exists with a header + rows
    drift = (tmp_path / "recording_drift.csv").read_text().strip().splitlines()
    assert drift[0] == "sample_index,host_perf_time"
    assert len(drift) > 1


def test_writer_ignores_short_and_nonmic(tmp_path):
    packets = [b"\x01\x00", ppg_packet()]  # too-short + ppg, then real data
    frames = make_frames(64)
    packets += chop_into_packets(frames.tobytes(), sizes=[240])
    _run_writer(tmp_path, packets)
    back, _ = sf.read(str(tmp_path / "recording.flac"), dtype="int16")
    assert back.shape == (64, 4)
    assert np.array_equal(back, frames)


# --- BLE drop-counting (no hardware; exercises the callback path) ----------

def test_ble_notify_drop_counting():
    q: "queue.Queue" = queue.Queue(maxsize=3)
    w = BleWorker(q)
    w._recording = True  # simulate active recording without a live loop
    pkt = protocol.HEADER_MIC.to_bytes(4, "little") + bytes(240)
    for _ in range(10):
        w._notify_cb(None, bytearray(pkt))
    # queue capped at 3; the rest were drop-oldest replacements
    assert q.qsize() == 3
    assert w.dropped_packets == 10 - 3


def test_ble_notify_ignored_when_not_recording():
    q: "queue.Queue" = queue.Queue(maxsize=10)
    w = BleWorker(q)
    w._recording = False
    pkt = protocol.HEADER_MIC.to_bytes(4, "little") + bytes(240)
    w._notify_cb(None, bytearray(pkt))
    assert q.qsize() == 0
