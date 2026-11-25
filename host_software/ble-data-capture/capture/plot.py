"""Live view of the four microphone channels (pyqtgraph).

The view reads only from the ring buffer, on a Qt timer, and never touches the
BLE link or the writer.

  * The x axis is a 10 s sliding window in stream time.
  * Each curve is min/max decimated before drawing to keep redraws cheap.
  * The y range of each channel follows the signal, smoothed over frames so the
    axes do not jump on every redraw.
"""

from __future__ import annotations

import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore

from . import protocol
from .ringbuffer import RingBuffer

_PENS = ("#1f77b4", "#ff7f0e", "#2ca02c", "#d62728")

# Maximum points per curve per redraw. A 10 s window at 4 kHz is 40k samples;
# drawing all of them on four curves takes GIL time away from the capture
# threads. Min/max decimation to a few thousand points keeps the peaks and is
# much cheaper to render.
_MAX_POINTS = 3000


def _decimate_minmax(
    x: np.ndarray, y: np.ndarray, max_points: int
) -> tuple[np.ndarray, np.ndarray]:
    """Min/max decimation that keeps peaks.

    The series is split into ``max_points // 2`` equal buckets and the min and
    max of each bucket are kept in time order. Short inputs are returned
    unchanged.
    """
    n = y.shape[0]
    if n <= max_points:
        return x, y
    buckets = max(1, max_points // 2)
    per = n // buckets
    if per < 2:
        return x, y
    usable = per * buckets
    yb = y[:usable].reshape(buckets, per)
    xb = x[:usable].reshape(buckets, per)
    rows = np.arange(buckets)
    amin = yb.argmin(axis=1)
    amax = yb.argmax(axis=1)
    ymin = yb[rows, amin]
    ymax = yb[rows, amax]
    xmin = xb[rows, amin]
    xmax = xb[rows, amax]
    # Keep the two extremes of each bucket in time order.
    first_is_min = amin <= amax
    out_x = np.empty(buckets * 2, dtype=x.dtype)
    out_y = np.empty(buckets * 2, dtype=y.dtype)
    out_x[0::2] = np.where(first_is_min, xmin, xmax)
    out_x[1::2] = np.where(first_is_min, xmax, xmin)
    out_y[0::2] = np.where(first_is_min, ymin, ymax)
    out_y[1::2] = np.where(first_is_min, ymax, ymin)
    return out_x, out_y


class LiveView:
    """A pyqtgraph widget showing the most recent ``window_s`` of audio.

    Args:
        ring: The ring buffer fed by the writer.
        window_s: Width of the sliding window in seconds.
        fps: Target redraw rate.
        y_smoothing: EMA factor (0..1) for the y range; higher reacts faster.
    """

    def __init__(
        self,
        ring: RingBuffer,
        window_s: float = 10.0,
        fps: int = 15,
        y_smoothing: float = 0.2,
    ) -> None:
        self._ring = ring
        self._window_s = float(window_s)
        self._window_frames = int(window_s * protocol.SAMPLE_RATE)
        self._fps = int(fps)
        self._alpha = float(y_smoothing)

        pg.setConfigOptions(antialias=False, background="w", foreground="k")
        self.widget = pg.GraphicsLayoutWidget()
        self.widget.setWindowTitle("APNEA_V1 - live microphone view")

        self._curves: list[pg.PlotDataItem] = []
        self._plots: list[pg.PlotItem] = []
        self._yrange: list[tuple[float, float] | None] = [None] * protocol.N_CHANNELS

        for i, label in enumerate(protocol.CHANNEL_LABELS):
            p = self.widget.addPlot(row=i, col=0)
            p.showGrid(x=True, y=True, alpha=0.2)
            p.setLabel("left", label)
            p.setMouseEnabled(x=False, y=False)
            p.setMenuEnabled(False)
            p.setXRange(0, self._window_s, padding=0)
            if i < protocol.N_CHANNELS - 1:
                p.getAxis("bottom").setStyle(showValues=False)
            else:
                p.setLabel("bottom", "Time", units="s")
            curve = p.plot(pen=pg.mkPen(_PENS[i % len(_PENS)], width=1))
            # Data is already min/max decimated before setData, so pyqtgraph's
            # own downsampling is turned off.
            curve.setDownsampling(auto=False)
            self._curves.append(curve)
            self._plots.append(p)

        self._timer = QtCore.QTimer()
        self._timer.timeout.connect(self._update)

    def start(self) -> None:
        self._timer.start(max(1, int(1000 / self._fps)))

    def stop(self) -> None:
        self._timer.stop()

    def _update(self) -> None:
        data = self._ring.read_last(self._window_frames)
        m = data.shape[0]
        if m == 0:
            return
        total = self._ring.total_written
        # Stream time of the samples in view.
        end_t = total / protocol.SAMPLE_RATE
        start_idx = total - m
        x = np.arange(start_idx, total, dtype=np.float64) / protocol.SAMPLE_RATE
        x0 = max(0.0, end_t - self._window_s)

        for c in range(protocol.N_CHANNELS):
            y = data[:, c].astype(np.float32)
            dx, dy = _decimate_minmax(x, y, _MAX_POINTS)
            self._curves[c].setData(dx, dy)
            self._plots[c].setXRange(x0, max(self._window_s, end_t), padding=0)
            self._apply_yrange(c, dy)

    def _apply_yrange(self, c: int, y: np.ndarray) -> None:
        lo = float(np.min(y))
        hi = float(np.max(y))
        if hi - lo < 1.0:  # flat signal: use a minimal range
            mid = 0.5 * (lo + hi)
            lo, hi = mid - 1.0, mid + 1.0
        margin = 0.1 * (hi - lo)
        lo -= margin
        hi += margin
        prev = self._yrange[c]
        if prev is None:
            new = (lo, hi)
        else:
            a = self._alpha
            new = (
                (1 - a) * prev[0] + a * lo,
                (1 - a) * prev[1] + a * hi,
            )
        self._yrange[c] = new
        self._plots[c].setYRange(new[0], new[1], padding=0)
