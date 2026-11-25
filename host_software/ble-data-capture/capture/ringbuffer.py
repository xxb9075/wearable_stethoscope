"""Thread-safe fixed-size ring buffer for the live plot.

The writer thread appends de-interleaved microphone frames; the GUI thread
periodically reads the most recent ``N`` seconds. Only the last window is ever
read, so a simple circular numpy array with a lock is sufficient and cheap.
"""

from __future__ import annotations

import threading

import numpy as np


class RingBuffer:
    """Circular buffer holding the most recent frames across all channels.

    A "frame" is one simultaneous sample from every channel, i.e. a row of
    shape ``(n_channels,)``.

    Args:
        capacity: Number of frames to retain.
        n_channels: Number of channels per frame.
        dtype: Sample dtype (defaults to int16 to match the wire format).
    """

    def __init__(
        self,
        capacity: int,
        n_channels: int,
        dtype: np.dtype | str = "<i2",
    ) -> None:
        self._capacity = int(capacity)
        self._n_channels = int(n_channels)
        self._buf = np.zeros((self._capacity, self._n_channels), dtype=dtype)
        self._write = 0          # index of next write position
        self._filled = 0         # number of valid frames (<= capacity)
        self._total = 0          # cumulative frames ever written
        self._lock = threading.Lock()

    @property
    def total_written(self) -> int:
        """Total number of frames ever appended (monotonic)."""
        with self._lock:
            return self._total

    def append(self, frames: np.ndarray) -> None:
        """Append a block of frames, overwriting the oldest as needed.

        Args:
            frames: Array of shape ``(n, n_channels)``.
        """
        if frames.ndim != 2 or frames.shape[1] != self._n_channels:
            raise ValueError(
                f"expected (n, {self._n_channels}) frames, got {frames.shape}"
            )
        n = frames.shape[0]
        if n == 0:
            return
        with self._lock:
            if n >= self._capacity:
                # Only the last `capacity` frames survive.
                self._buf[:] = frames[-self._capacity:]
                self._write = 0
                self._filled = self._capacity
            else:
                end = self._write + n
                if end <= self._capacity:
                    self._buf[self._write:end] = frames
                else:
                    first = self._capacity - self._write
                    self._buf[self._write:] = frames[:first]
                    self._buf[: end - self._capacity] = frames[first:]
                self._write = end % self._capacity
                self._filled = min(self._filled + n, self._capacity)
            self._total += n

    def read_last(self, n_frames: int) -> np.ndarray:
        """Return a contiguous copy of the most recent ``n_frames`` frames.

        Fewer frames are returned if the buffer is not yet full.

        Args:
            n_frames: Maximum number of most-recent frames to return.

        Returns:
            Array of shape ``(m, n_channels)`` with ``m <= n_frames`` in
            chronological (oldest-first) order.
        """
        n_frames = min(int(n_frames), self._capacity)
        with self._lock:
            m = min(n_frames, self._filled)
            if m == 0:
                return np.empty((0, self._n_channels), dtype=self._buf.dtype)
            start = (self._write - m) % self._capacity
            end = start + m
            if end <= self._capacity:
                return self._buf[start:end].copy()
            first = self._capacity - start
            out = np.empty((m, self._n_channels), dtype=self._buf.dtype)
            out[:first] = self._buf[start:]
            out[first:] = self._buf[: m - first]
            return out
