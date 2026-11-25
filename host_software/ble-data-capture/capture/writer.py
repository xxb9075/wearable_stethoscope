"""Streaming writer: BLE payloads to de-interleaved, lossless FLAC.

The writer has its own thread, so BLE notification handling and the Qt GUI
never wait on disk I/O. It

  * reads ``(payload_bytes, host_perf_time)`` items from a queue,
  * de-interleaves the 4-channel int16 microphone stream, keeping leftover
    bytes so frames split across packets are joined correctly,
  * appends the raw samples to a FLAC file as they arrive (only the small
    plotting ring buffer is kept in memory),
  * feeds the live-plot ring buffer,
  * writes timing metadata (absolute start time, a drift log and gap events)
    so the recording can be aligned with another instrument later.

ADC codes are stored unchanged. DC removal is done offline in
:mod:`postprocess`.
"""

from __future__ import annotations

import csv
import json
import queue
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

import numpy as np
import soundfile as sf

from . import protocol

# A recording is marked "degraded" if the host dropped packets or the estimated
# sample loss is above this percentage. The threshold sits above the ~0.4 %
# crystal offset of the device, so a clean recording stays "ok" while real
# packet loss (several percent) is flagged.
_LOSS_PCT_DEGRADED = 1.5

# Put on the queue to tell the writer thread to finish.
STOP = object()

# Drift-log interval and flush interval, in seconds of stream time. At 0.1 s the
# drift log stays at a few MB for a full night and still gives the rate fit
# enough points.
_DRIFT_INTERVAL_S: float = 0.1
_FLUSH_INTERVAL_S: float = 2.0


class Writer(threading.Thread):
    """Write BLE payloads to a FLAC file plus metadata files.

    Args:
        data_queue: Queue of ``(payload: bytes, t_recv: float)`` items, where
            ``t_recv`` is a :func:`time.perf_counter` timestamp taken in the BLE
            notification callback. Push :data:`STOP` to end the session.
        ring: Ring buffer that the GUI reads for the live plot.
        session_dir: Directory to write ``recording.flac`` / ``recording.json``
            / ``recording_drift.csv`` into.
        sw_version: Version string recorded in metadata.
        dropped_getter: Optional callable that returns the BLE worker's
            dropped-packet count; it is read once when the file is closed.
    """

    def __init__(
        self,
        data_queue: "queue.Queue",
        ring,
        session_dir: Path,
        sw_version: str = "1.0.0",
        dropped_getter: Callable[[], int] | None = None,
    ) -> None:
        super().__init__(name="Writer", daemon=True)
        self._queue = data_queue
        self._ring = ring
        self._dir = Path(session_dir)
        self._sw_version = sw_version
        self._dropped_getter = dropped_getter
        # Short data-quality summary, filled in by _finalize().
        self.summary: dict | None = None

        self._flac_path = self._dir / "recording.flac"
        self._json_path = self._dir / "recording.json"
        self._drift_path = self._dir / "recording_drift.csv"

        # Bytes left over that do not make a whole frame yet.
        self._carry = bytearray()

        # Timing reference (set on the first microphone packet).
        self._perf0: float | None = None   # perf_counter at session open
        self._wall0: float | None = None   # time.time() paired with _perf0
        self._first_perf: float | None = None
        self._start_utc: str | None = None
        self._start_unix: float | None = None

        # Counters and logs
        self._total_frames = 0
        self._mic_packets = 0
        self._other_packets = 0
        self._next_drift_at = 0.0
        self._gaps: list[dict] = []
        self._gap_lock = threading.Lock()

        self._sf: sf.SoundFile | None = None
        self._drift_file = None
        self._drift_writer = None
        self._last_flush = 0.0
        self._finalized = False

    # Public API (called from other threads)

    def mark_gap(self, approx_lost_ms: float | None = None) -> None:
        """Record a gap in the stream, e.g. after a BLE reconnect.

        Thread-safe; can be called from the BLE thread.

        Args:
            approx_lost_ms: Best estimate of the lost span in milliseconds, or
                ``None`` if unknown.
        """
        with self._gap_lock:
            self._gaps.append(
                {
                    "sample_index": self._total_frames,
                    "utc": datetime.now(timezone.utc).isoformat(),
                    "approx_lost_ms": approx_lost_ms,
                }
            )

    # Thread body

    def run(self) -> None:
        self._dir.mkdir(parents=True, exist_ok=True)
        # Pair a wall-clock reading with a perf_counter reading so that any
        # perf_counter timestamp can be converted to UTC.
        self._perf0 = time.perf_counter()
        self._wall0 = time.time()
        self._sf = sf.SoundFile(
            str(self._flac_path),
            mode="w",
            samplerate=protocol.SAMPLE_RATE,
            channels=protocol.N_CHANNELS,
            subtype="PCM_16",
            format="FLAC",
        )
        self._drift_file = open(self._drift_path, "w", newline="")
        self._drift_writer = csv.writer(self._drift_file)
        self._drift_writer.writerow(["sample_index", "host_perf_time"])
        self._last_flush = time.perf_counter()

        try:
            while True:
                item = self._queue.get()
                if item is STOP:
                    break
                payload, t_recv = item
                self._handle_payload(payload, t_recv)
        finally:
            self._finalize()

    # Internals

    def _handle_payload(self, payload: bytes, t_recv: float) -> None:
        if len(payload) < protocol.HEADER_LEN:
            return
        if protocol.parse_header(payload) != protocol.HEADER_MIC:
            self._other_packets += 1
            return

        self._mic_packets += 1
        if self._first_perf is None:
            self._first_perf = t_recv
            self._start_unix = self._wall0 + (t_recv - self._perf0)
            self._start_utc = datetime.fromtimestamp(
                self._start_unix, tz=timezone.utc
            ).isoformat()

        # Append to the leftover bytes and take out whole 4-channel frames.
        self._carry += payload[protocol.HEADER_LEN:]
        n_frames = len(self._carry) // protocol.BYTES_PER_FRAME
        if n_frames == 0:
            return
        nbytes = n_frames * protocol.BYTES_PER_FRAME
        frames = np.frombuffer(
            bytes(self._carry[:nbytes]), dtype=protocol.SAMPLE_DTYPE
        ).reshape(n_frames, protocol.N_CHANNELS)
        del self._carry[:nbytes]

        self._sf.write(frames)
        self._ring.append(frames)
        self._total_frames += n_frames

        # Drift log, one row per _DRIFT_INTERVAL_S of stream time.
        stream_t = self._total_frames / protocol.SAMPLE_RATE
        if stream_t >= self._next_drift_at:
            self._drift_writer.writerow([self._total_frames, f"{t_recv:.6f}"])
            self._next_drift_at = stream_t + _DRIFT_INTERVAL_S

        now = time.perf_counter()
        if now - self._last_flush >= _FLUSH_INTERVAL_S:
            self._sf.flush()
            self._drift_file.flush()
            self._last_flush = now

    def _finalize(self) -> None:
        if self._finalized:
            return
        self._finalized = True
        if self._sf is not None:
            self._sf.flush()
            self._sf.close()
        if self._drift_file is not None:
            self._drift_file.flush()
            self._drift_file.close()

        duration_s = self._total_frames / protocol.SAMPLE_RATE
        stats = self._stream_stats()
        dropped = 0
        if self._dropped_getter is not None:
            try:
                dropped = int(self._dropped_getter())
            except Exception:
                dropped = 0
        loss_pct = stats["estimated_loss_pct"] or 0.0
        degraded = dropped > 0 or loss_pct > _LOSS_PCT_DEGRADED
        quality = "degraded" if degraded else "ok"
        meta = {
            "sw_version": self._sw_version,
            "device_name": protocol.DEVICE_NAME,
            "format": "FLAC",
            "subtype": "PCM_16",
            "sample_rate": protocol.SAMPLE_RATE,
            "n_channels": protocol.N_CHANNELS,
            "channel_order": list(protocol.CHANNEL_LABELS),
            "dtype": "int16",
            "bit_depth": protocol.BIT_DEPTH,
            "bit_depth_note": "14-bit differential ADC codes in int16 container",
            "start_time_utc": self._start_utc,
            "start_time_unix": self._start_unix,
            "total_samples": self._total_frames,
            "duration_s": duration_s,
            "measured_mean_rate_hz": stats["measured_mean_rate_hz"],
            "max_jitter_ms": stats["max_jitter_ms"],
            "host_span_s": stats["host_span_s"],
            "mic_packets": self._mic_packets,
            "non_mic_packets_ignored": self._other_packets,
            "dropped_packets": dropped,
            "stall_count": stats["stall_count"],
            "estimated_sample_loss": stats["estimated_sample_loss"],
            "estimated_loss_pct": stats["estimated_loss_pct"],
            "quality": quality,
            "gaps": self._gaps,
            "files": {
                "audio": self._flac_path.name,
                "drift_log": self._drift_path.name,
            },
        }
        with open(self._json_path, "w") as f:
            json.dump(meta, f, indent=2)

        self.summary = {
            "quality": quality,
            "dropped_packets": dropped,
            "estimated_loss_pct": stats["estimated_loss_pct"],
            "stall_count": stats["stall_count"],
            "measured_mean_rate_hz": stats["measured_mean_rate_hz"],
            "gaps": len(self._gaps),
        }

    def _stream_stats(self) -> dict:
        """Estimate sample rate, jitter and data loss from the drift log.

        Data arrives in bursts of about 150 ms, so a rate taken from the end
        points or from single intervals is noisy. The delivered rate is taken
        as the least-squares slope of sample index against arrival time; stalls
        and the catch-up bursts that follow them cancel out in the fit.

        ``estimated_loss_pct`` is how far the delivered rate is below the
        nominal rate. The device crystal runs about 0.4 % slow, so a clean
        recording shows about 0.4 %; real packet loss on a congested link shows
        up as several percent. ``dropped_packets`` (host queue overflow) is a
        separate, direct loss count.
        """
        out: dict = {
            "measured_mean_rate_hz": None,
            "max_jitter_ms": None,
            "host_span_s": None,
            "stall_count": 0,
            "estimated_sample_loss": 0,
            "estimated_loss_pct": 0.0,
        }
        try:
            data = np.genfromtxt(
                self._drift_path, delimiter=",", skip_header=1, dtype=float
            )
        except (OSError, ValueError):
            return out
        if data.ndim != 2 or data.shape[0] < 2:
            return out
        idx = data[:, 0]
        t = data[:, 1]
        host_span = float(t[-1] - t[0])
        if host_span <= 0:
            return out
        # Delivered rate: slope of idx = rate * t + b.
        slope = float(np.polyfit(t, idx, 1)[0])
        d_idx = np.diff(idx)
        d_t = np.diff(t)
        # Jitter: largest difference between an interval's measured duration and
        # the duration expected at the nominal sample rate.
        expected_t = d_idx / protocol.SAMPLE_RATE
        max_jitter_ms = float(np.max(np.abs(d_t - expected_t)) * 1000.0)
        # Stalls: intervals much longer than the typical spacing between rows.
        med_dt = float(np.median(d_t)) if d_t.size else 0.0
        stall_thresh = max(0.3, 5.0 * med_dt)
        stall_count = int(np.sum(d_t > stall_thresh))
        # Loss: how far the delivered rate is below nominal over the span.
        nominal = float(protocol.SAMPLE_RATE)
        loss_pct = max(0.0, (nominal - slope) / nominal * 100.0)
        est_loss = max(0.0, host_span * nominal - float(idx[-1] - idx[0]))
        out.update(
            measured_mean_rate_hz=slope,
            max_jitter_ms=max_jitter_ms,
            host_span_s=round(host_span, 3),
            stall_count=stall_count,
            estimated_sample_loss=int(round(est_loss)),
            estimated_loss_pct=round(loss_pct, 2),
        )
        return out
