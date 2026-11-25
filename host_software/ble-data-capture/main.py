"""Host capture app for the APNEA_V1 device.

``python ble-data-capture/main.py`` finds and connects to the device, shows
the four microphone channels live, and records them to a lossless FLAC file
with timing metadata. The GUI has two buttons, Start and Stop; a recording is
saved when it is stopped, and closing the window exits.

For unattended runs use ``--headless`` with ``--duration``::

    python ble-data-capture/main.py --headless --duration 3600

By default only the raw ADC codes are saved (FLAC plus the drift log and
metadata). ``--postprocess`` also runs DC removal and writes
``*_MIC_post.csv`` after the recording ends; other options are in
``postprocess.py``.

Output goes to ``<repo>/data/<YYYY-MM-DD_THH-MM-SS>/``.
"""

from __future__ import annotations

import argparse
import queue
import sys
import time
from datetime import datetime
from pathlib import Path

# Make the capture package importable when run as a script.
sys.path.insert(0, str(Path(__file__).resolve().parent))

from capture import __version__, protocol
from capture.ble import BleWorker
from capture.btreset import reset_bluetooth
from capture.ringbuffer import RingBuffer
from capture.writer import STOP, Writer

# Bounded queue (~15 s of packets at ~133 packets/s) so a stalled writer cannot
# grow memory without limit. On overflow the oldest packet is dropped and
# counted.
_QUEUE_MAXSIZE = 4000
# Ring buffer length: a bit more than the 10 s plot window.
_RING_SECONDS = 12


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _run_postprocess(session_dir: Path | None) -> None:
    """Write the DC-removed FLAC and the CSV for a session.

    The raw FLAC is not modified; ``recording_dcremoved.flac`` and
    ``<session>_MIC_post.csv`` are added next to it.
    """
    if session_dir is None:
        return
    flac = Path(session_dir) / "recording.flac"
    if not flac.exists():
        print(f"[postprocess] no FLAC at {flac}; skipping.", flush=True)
        return
    from postprocess import dc_remove, to_csv

    print(f"[postprocess] DC-removing {flac.name} + writing CSV...", flush=True)
    dc_remove(flac)
    to_csv(flac)
    print("[postprocess] done.", flush=True)


class CaptureController:
    """Holds the queue, ring buffer, BLE worker and the writer of a session."""

    def __init__(self, out_dir: Path, status_cb=None, ble_factory=None) -> None:
        self._out_dir = Path(out_dir)
        self._queue: "queue.Queue" = queue.Queue(maxsize=_QUEUE_MAXSIZE)
        self.ring = RingBuffer(
            capacity=_RING_SECONDS * protocol.SAMPLE_RATE,
            n_channels=protocol.N_CHANNELS,
        )
        self._status_cb = status_cb or (lambda s, m: None)
        self.writer: Writer | None = None
        self.session_dir: Path | None = None
        self.recording = False
        self.last_summary: dict | None = None
        self._record_start: float | None = None
        # ble_factory lets the tests use a fake device; by default the real BLE
        # worker is used. Bluetooth reset is passed in as the recovery action
        # for a stuck link.
        factory = ble_factory or BleWorker
        self.ble = factory(
            self._queue,
            on_status=self._status_cb,
            on_gap=self._on_gap,
            on_recover=reset_bluetooth,
        )

    def start_worker(self) -> None:
        self.ble.start()
        self.ble.wait_ready(timeout=5.0)

    def _on_gap(self) -> None:
        if self.writer is not None:
            self.writer.mark_gap(approx_lost_ms=None)

    def start_recording(self) -> Path:
        if self.recording:
            return self.session_dir  # type: ignore[return-value]
        session = datetime.now().strftime("%Y-%m-%d_T%H-%M-%S")
        self.session_dir = self._out_dir / session
        self.writer = Writer(
            self._queue,
            self.ring,
            self.session_dir,
            sw_version=__version__,
            dropped_getter=lambda: self.ble.dropped_packets,
        )
        self.writer.start()
        self.ble.start_recording()
        self.recording = True
        self._record_start = time.perf_counter()
        return self.session_dir

    def stop_recording(self) -> Path | None:
        if not self.recording:
            return None
        self.ble.stop_recording()
        self._queue.put(STOP)
        if self.writer is not None:
            self.writer.join(timeout=30.0)
        self.recording = False
        session = self.session_dir
        # Report a degraded recording right away (details in recording.json).
        self.last_summary = self.writer.summary if self.writer is not None else None
        if self.last_summary and self.last_summary["quality"] == "degraded":
            s = self.last_summary
            self._status_cb(
                "warning",
                f"Data quality DEGRADED: ~{s['estimated_loss_pct']:.1f}% sample "
                f"loss, {s['dropped_packets']} dropped packets, "
                f"{s['stall_count']} stalls. Prefer --headless for clean capture.",
            )
        self.writer = None
        return session

    def elapsed_s(self) -> float:
        if self._record_start is None or not self.recording:
            return 0.0
        return time.perf_counter() - self._record_start

    def shutdown(self) -> None:
        if self.recording:
            self.stop_recording()
        self.ble.shutdown()
        self.ble.join(timeout=5.0)


# GUI mode

def run_gui(out_dir: Path, duration: float | None, postprocess: bool = False) -> int:
    from pyqtgraph.Qt import QtCore, QtWidgets

    from capture.plot import LiveView

    app = QtWidgets.QApplication(sys.argv)

    class StatusBridge(QtCore.QObject):
        status = QtCore.Signal(str, str)

    bridge = StatusBridge()

    def _emit_status(statename: str, message: str) -> None:
        # Print to the console as well, then pass to the Qt thread for the
        # status label.
        print(f"[{statename}] {message}", file=sys.stderr, flush=True)
        bridge.status.emit(statename, message)

    ctrl = CaptureController(out_dir, status_cb=_emit_status)

    view = LiveView(ctrl.ring)

    win = QtWidgets.QMainWindow()
    win.setWindowTitle("APNEA_V1 Capture")
    central = QtWidgets.QWidget()
    layout = QtWidgets.QVBoxLayout(central)

    controls = QtWidgets.QHBoxLayout()
    start_btn = QtWidgets.QPushButton("Start")
    start_btn.setEnabled(False)  # enabled once the device is ready
    stop_btn = QtWidgets.QPushButton("Stop")
    stop_btn.setEnabled(False)
    conn_lbl = QtWidgets.QLabel("Starting...")
    rx_lbl = QtWidgets.QLabel("Rx: -")
    elapsed_lbl = QtWidgets.QLabel("00:00:00")
    controls.addWidget(start_btn)
    controls.addWidget(stop_btn)
    controls.addSpacing(16)
    controls.addWidget(QtWidgets.QLabel("Status:"))
    controls.addWidget(conn_lbl)
    # Stretch after the status label so the Rx and elapsed-time readouts stay
    # at the right edge.
    controls.addStretch(1)
    controls.addWidget(rx_lbl)
    controls.addSpacing(16)
    controls.addWidget(elapsed_lbl)

    layout.addLayout(controls)
    layout.addWidget(view.widget, stretch=1)
    win.setCentralWidget(central)
    win.resize(1000, 720)

    last_session: dict[str, Path | None] = {"dir": None}

    def on_status(statename: str, message: str) -> None:
        # Update the status label. The Start button follows ble.ready
        # (connected and subscribed); the ticker checks it again as well.
        conn_lbl.setText(message)
        start_btn.setEnabled(ctrl.ble.ready and not ctrl.recording)

    bridge.status.connect(on_status)

    def do_start() -> None:
        session = ctrl.start_recording()
        rx_state["last_frames"] = ctrl.ring.total_written
        rx_state["last_t"] = time.perf_counter()
        start_btn.setEnabled(False)
        stop_btn.setEnabled(True)
        conn_lbl.setText(f"Recording -> {session.name}")

    def do_stop() -> None:
        session = ctrl.stop_recording()
        stop_btn.setEnabled(False)
        start_btn.setEnabled(ctrl.ble.ready)
        if session is not None:
            last_session["dir"] = session
            # Show the session name; the full path is printed to the console.
            msg = f"Saved -> {session.name}"
            s = ctrl.last_summary
            if s and s["quality"] == "degraded":
                msg += f"  WARNING: ~{s['estimated_loss_pct']:.1f}% loss"
            conn_lbl.setText(msg)

    start_btn.clicked.connect(do_start)
    stop_btn.clicked.connect(do_stop)

    # Elapsed time and received-data rate, so it is visible when the device is
    # connected but not streaming.
    ticker = QtCore.QTimer()
    rx_state = {"last_frames": 0, "last_t": time.perf_counter()}

    def tick() -> None:
        secs = int(ctrl.elapsed_s())
        elapsed_lbl.setText(time.strftime("%H:%M:%S", time.gmtime(secs)))
        if ctrl.recording:
            now = time.perf_counter()
            frames = ctrl.ring.total_written
            dt = now - rx_state["last_t"]
            rate = (frames - rx_state["last_frames"]) / dt if dt > 0 else 0.0
            rx_state["last_frames"] = frames
            rx_state["last_t"] = now
            if rate < 1.0:
                rx_lbl.setText("Rx: no data - check device")
            else:
                rx_lbl.setText(f"Rx: {frames:,} frames  {rate:,.0f}/s")
        else:
            rx_lbl.setText("Rx: -")
            # Check readiness here too, so a missed status event cannot leave
            # Start enabled.
            start_btn.setEnabled(ctrl.ble.ready)
        if duration is not None and ctrl.recording and secs >= duration:
            do_stop()
            win.close()

    ticker.timeout.connect(tick)
    ticker.start(500)

    # With --duration, start recording as soon as the device is ready.
    auto_started = {"done": False}

    def maybe_autostart() -> None:
        if duration is not None and ctrl.ble.ready and not auto_started["done"]:
            auto_started["done"] = True
            do_start()

    autostart_timer = QtCore.QTimer()
    autostart_timer.timeout.connect(maybe_autostart)
    autostart_timer.start(200)

    def on_close(event) -> None:
        ticker.stop()
        view.stop()
        ctrl.shutdown()
        event.accept()

    win.closeEvent = on_close  # type: ignore[method-assign]

    ctrl.start_worker()
    view.start()
    win.show()
    rc = app.exec()
    if postprocess:
        _run_postprocess(last_session["dir"])
    return rc


# Headless mode (unattended runs)

def run_headless(
    out_dir: Path,
    duration: float | None,
    postprocess: bool = False,
    connect_timeout: float = 300.0,
) -> int:
    connected = {"ok": False}

    def on_status(statephase: str, message: str) -> None:
        print(f"[{statephase}] {message}", flush=True)
        if statephase == "connected":
            connected["ok"] = True
        elif statephase in ("disconnected",):
            connected["ok"] = False

    ctrl = CaptureController(out_dir, status_cb=on_status)
    ctrl.start_worker()

    # Allow a long first connect: the device may still be releasing a previous
    # half-open session. The BLE worker keeps retrying in the meantime.
    print(f"Waiting for connection (up to {connect_timeout:.0f}s)...", flush=True)
    t0 = time.time()
    while not connected["ok"]:
        if time.time() - t0 > connect_timeout:
            print(
                f"No connection within {connect_timeout:.0f}s; giving up.",
                flush=True,
            )
            ctrl.shutdown()
            return 1
        time.sleep(0.5)

    session = ctrl.start_recording()
    print(f"Recording -> {session}", flush=True)

    try:
        import psutil  # optional, for memory monitoring on long runs

        proc = psutil.Process()
    except Exception:
        proc = None

    def rss_mb() -> float:
        return proc.memory_info().rss / 1e6 if proc is not None else 0.0

    baseline_rss = rss_mb()
    peak_rss = baseline_rss
    try:
        start = time.time()
        last_log = -1
        while True:
            elapsed = time.time() - start
            if duration is not None and elapsed >= duration:
                break
            secs = int(elapsed)
            if secs % 30 == 0 and secs != last_log:
                last_log = secs
                rss = rss_mb()
                peak_rss = max(peak_rss, rss)
                print(
                    f"  t={secs}s frames={ctrl.ring.total_written} "
                    f"dropped={ctrl.ble.dropped_packets} "
                    f"rss={rss:.1f}MB (base {baseline_rss:.1f}, peak {peak_rss:.1f})",
                    flush=True,
                )
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("Interrupted - stopping.", flush=True)
    finally:
        session = ctrl.stop_recording()
        ctrl.shutdown()
        final_rss = rss_mb()
        s = ctrl.last_summary or {}
        quality = s.get("quality", "unknown")
        print(
            f"Saved -> {session}\n"
            f"Quality: {quality} - ~{s.get('estimated_loss_pct', 0.0):.1f}% loss, "
            f"{s.get('dropped_packets', 0)} dropped, {s.get('stall_count', 0)} stalls, "
            f"{s.get('gaps', 0)} gaps, rate {s.get('measured_mean_rate_hz') or 0:.0f} Hz\n"
            f"RSS: baseline {baseline_rss:.1f}MB, peak {peak_rss:.1f}MB, "
            f"final {final_rss:.1f}MB (growth {final_rss - baseline_rss:+.1f}MB)",
            flush=True,
        )
    if postprocess:
        _run_postprocess(session)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="APNEA_V1 microphone capture")
    parser.add_argument(
        "--out",
        type=Path,
        default=_repo_root() / "data",
        help="Base output directory (default: <repo>/data).",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=None,
        help="Stop automatically after N seconds.",
    )
    parser.add_argument(
        "--headless",
        action="store_true",
        help="Run without the GUI, e.g. for timed runs with --duration.",
    )
    parser.add_argument(
        "--postprocess",
        action="store_true",
        help="After recording, write a DC-removed FLAC and "
        "<session>_MIC_post.csv. Off by default (raw FLAC only).",
    )
    args = parser.parse_args(argv)

    # Windows consoles default to cp1252; switch to UTF-8 for the status text.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")  # type: ignore[union-attr]
        except (AttributeError, ValueError):
            pass

    args.out.mkdir(parents=True, exist_ok=True)

    if args.headless:
        return run_headless(args.out, args.duration, postprocess=args.postprocess)
    return run_gui(args.out, args.duration, postprocess=args.postprocess)


if __name__ == "__main__":
    raise SystemExit(main())
