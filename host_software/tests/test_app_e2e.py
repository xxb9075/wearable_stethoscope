"""End-to-end app test driving the full CaptureController with a fake device.

Exercises the record -> auto-save lifecycle (queue, writer, ring buffer,
metadata, gap handling, clean shutdown) without any BLE hardware by injecting a
fake worker that emits firmware-accurate packets.
"""

from __future__ import annotations

import importlib.util
import json
import queue
import threading
import time
from pathlib import Path

import numpy as np
import soundfile as sf

from capture import protocol

from helpers import make_frames, mic_packet, ppg_packet

# main.py lives at ble-data-capture/main.py (not a package module); load it.
_MAIN_PATH = Path(__file__).resolve().parent.parent / "ble-data-capture" / "main.py"
_spec = importlib.util.spec_from_file_location("apnea_main", _MAIN_PATH)
apnea_main = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(apnea_main)


class FakeBleWorker:
    """Drop-in stand-in for BleWorker that streams synthetic bursts."""

    def __init__(self, data_queue, on_status=None, on_gap=None, on_recover=None):
        self._q = data_queue
        self._on_status = on_status or (lambda s, m: None)
        self._on_gap = on_gap or (lambda: None)
        self._recording = False
        self._stop = False
        self.dropped_packets = 0
        self._thread = None
        self._frames = make_frames(protocol.SAMPLE_RATE * 60)
        self._i = 0

    @property
    def connected(self):
        return True

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def wait_ready(self, timeout=None):
        self._on_status("connected", "Connected to FAKE.")
        return True

    def _run(self):
        while not self._stop:
            if self._recording:
                burst = self._frames[self._i : self._i + 600]
                if burst.shape[0] < 600:
                    self._i = 0
                    continue
                self._i += 600
                raw = burst.tobytes()
                t = time.perf_counter()
                step = 30 * protocol.BYTES_PER_FRAME
                for p in range(0, len(raw), step):
                    self._q.put((mic_packet(raw[p : p + step]), t))
                self._q.put((ppg_packet(), t))
            time.sleep(0.15)

    def start_recording(self):
        self._recording = True

    def stop_recording(self):
        self._recording = False

    def simulate_reconnect_gap(self):
        self._on_gap()

    def shutdown(self):
        self._stop = True
        if self._thread:
            self._thread.join(timeout=2)

    def join(self, timeout=None):
        if self._thread:
            self._thread.join(timeout=timeout)


def test_full_record_save_cycle(tmp_path):
    ctrl = apnea_main.CaptureController(tmp_path, ble_factory=FakeBleWorker)
    ctrl.start_worker()
    session = ctrl.start_recording()
    assert ctrl.recording

    time.sleep(1.2)
    ctrl.ble.simulate_reconnect_gap()  # mid-recording discontinuity
    time.sleep(1.2)

    saved = ctrl.stop_recording()
    assert saved == session
    assert not ctrl.recording
    ctrl.shutdown()

    flac = session / "recording.flac"
    meta_path = session / "recording.json"
    assert flac.exists() and meta_path.exists()

    info = sf.info(str(flac))
    assert info.channels == 4
    assert info.samplerate == 4000
    assert info.frames > protocol.SAMPLE_RATE  # at least ~1 s captured

    meta = json.loads(meta_path.read_text())
    assert meta["total_samples"] == info.frames
    assert meta["duration_s"] > 1.0
    assert meta["start_time_utc"] is not None
    assert len(meta["gaps"]) == 1
    # ring buffer holds recent data for the live plot
    assert ctrl.ring.read_last(4000).shape[1] == 4


def test_controller_no_double_start(tmp_path):
    ctrl = apnea_main.CaptureController(tmp_path, ble_factory=FakeBleWorker)
    ctrl.start_worker()
    s1 = ctrl.start_recording()
    s2 = ctrl.start_recording()  # idempotent while recording
    assert s1 == s2
    time.sleep(0.5)
    ctrl.stop_recording()
    ctrl.shutdown()
    # stop again is a no-op
    assert ctrl.stop_recording() is None
