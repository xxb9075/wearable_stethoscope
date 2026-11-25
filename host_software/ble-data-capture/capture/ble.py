"""BLE worker for the APNEA_V1 device (bleak >= 3.0).

The worker runs its own asyncio event loop in a separate thread so that it does
not compete with the Qt GUI. It

  * scans for the ``APNEA_V1`` peripheral by name and connects,
  * subscribes to the data characteristic and puts each notification payload,
    with a monotonic receive timestamp, on a queue for the writer,
  * sends the start/stop commands,
  * reconnects after link failures. Every attempt creates a new
    :class:`BleakClient`; reusing a closed WinRT object raises
    ``[WinError -2147483629] The object has been closed`` on Windows. Retries
    use exponential backoff, and a reconnect during a recording is logged as a
    gap.

If the link keeps failing, the status callback asks the user to reset
Bluetooth or power-cycle the device instead of retrying silently.
"""

from __future__ import annotations

import asyncio
import queue
import threading
import time
from typing import Callable

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from . import protocol

# Reconnect and backoff settings
_SCAN_TIMEOUT_S = 8.0
_SETTLE_S = 2.0            # time for MTU / data length / PHY / conn-param updates
_CONNECT_TIMEOUT_S = 12.0  # per attempt; a healthy device connects in < 10 s
# Backoff, so that a half-open link left by a failed attempt can time out on
# the device (supervision timeout is about 6 s) before the next try.
_BACKOFF_START_S = 3.0
_BACKOFF_MAX_S = 8.0
# A stuck link (device found, but connect() times out) does not recover by
# retrying; only a Bluetooth reset clears it. Try recovery after a few failed
# connects.
_MAX_CONSECUTIVE_FAILURES = 3
# Toggling the radio too often can leave the Windows BT stack in a worse state.
# Resets are spaced out, and after two unsuccessful ones the user is asked to
# toggle Bluetooth or power-cycle the device by hand.
_RESET_COOLDOWN_S = 25.0
_MAX_AUTO_RESETS = 2

StatusCb = Callable[[str, str], None]  # (state, message)


class BleWorker(threading.Thread):
    """Manage the BLE link and stream notifications to a queue.

    Args:
        data_queue: Destination for ``(payload_bytes, t_recv)`` items, where
            ``t_recv`` is a :func:`time.perf_counter` timestamp captured in the
            notification callback.
        on_status: Optional callback ``(state, message)`` invoked (from the BLE
            thread) on connection-state changes. ``state`` is one of
            ``"scanning"``, ``"connecting"``, ``"connected"``, ``"disconnected"``,
            ``"error"``, ``"hardware"``.
        on_gap: Optional callback invoked when a reconnect during a recording
            creates a gap in the stream.
        on_recover: Optional blocking callable (e.g. a Bluetooth reset) tried
            when connects keep failing. It runs in an executor thread.
    """

    def __init__(
        self,
        data_queue: "queue.Queue",
        on_status: StatusCb | None = None,
        on_gap: Callable[[], None] | None = None,
        on_recover: Callable[[], None] | None = None,
    ) -> None:
        super().__init__(name="BleWorker", daemon=True)
        self._queue = data_queue
        self._on_status = on_status or (lambda s, m: None)
        self._on_gap = on_gap or (lambda: None)
        self._on_recover = on_recover

        self._loop: asyncio.AbstractEventLoop | None = None
        self._client: BleakClient | None = None
        self._notify_active = False  # set after start_notify succeeds
        self._recording = False
        self._shutdown = False
        self._dropped = 0
        self._connect_failures = 0
        self._resets_done = 0        # automatic resets since the last connection
        self._last_reset_t = 0.0     # perf_counter time of the last reset

        # asyncio objects are created inside the loop thread.
        self._disconnected_evt: asyncio.Event | None = None
        self._ready = threading.Event()  # set once the loop is running

    # Properties

    @property
    def dropped_packets(self) -> int:
        """Number of notifications dropped due to a full queue."""
        return self._dropped

    @property
    def connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    @property
    def ready(self) -> bool:
        """True when connected and subscribed to notifications.

        The GUI enables Start only when this is true. A GATT connection that is
        still settling (before ``start_notify``) does not count as ready.
        """
        return (
            self._client is not None
            and self._client.is_connected
            and self._notify_active
        )

    # Thread body

    def run(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._disconnected_evt = asyncio.Event()
        self._ready.set()
        try:
            self._loop.run_until_complete(self._main())
        finally:
            self._loop.close()

    async def _main(self) -> None:
        self._connect_failures = 0
        backoff = _BACKOFF_START_S
        while not self._shutdown:
            try:
                self._on_status("scanning", f"Scanning for {protocol.DEVICE_NAME}...")
                device = await BleakScanner.find_device_by_name(
                    protocol.DEVICE_NAME, timeout=_SCAN_TIMEOUT_S
                )
                if device is None:
                    # Device is not advertising (likely off). Keep scanning;
                    # a Bluetooth reset would not help here.
                    await self._note_failure(
                        f"{protocol.DEVICE_NAME} not found in scan",
                        backoff,
                        connect_failure=False,
                    )
                    backoff = min(backoff * 2, _BACKOFF_MAX_S)
                    continue

                await self._connect_and_serve(device)
                # A normal return means the device disconnected cleanly.
                self._connect_failures = 0
                backoff = _BACKOFF_START_S
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                # Catch everything: connect() can raise TimeoutError, which is
                # not a BleakError or OSError, and WinRT errors such as
                # "[WinError -2147483629] The object has been closed". The
                # reconnect loop has to keep running in all of these cases.
                await self._safe_disconnect()
                await self._note_failure(
                    f"connection attempt failed ({type(exc).__name__}: {exc})",
                    backoff,
                    connect_failure=True,
                )
                backoff = min(backoff * 2, _BACKOFF_MAX_S)

    async def _note_failure(
        self, reason: str, backoff: float, connect_failure: bool
    ) -> None:
        """Report a failed attempt, wait, and try recovery if the link is stuck.

        A stale OS session (device found, but ``connect()`` keeps timing out) is
        cleared with a Bluetooth reset. A device that is not advertising is
        most likely off, so we only keep scanning.
        """
        self._on_status("error", f"{reason}; retrying...")
        if not connect_failure:
            await asyncio.sleep(backoff)
            return

        self._connect_failures += 1
        # Consider a recovery after every few consecutive failures.
        if self._connect_failures % _MAX_CONSECUTIVE_FAILURES == 0:
            await self._maybe_recover()
        await asyncio.sleep(backoff)

    async def _maybe_recover(self) -> None:
        """Reset Bluetooth to clear a stuck link, with a cooldown and a limit.

        After the limit is reached the radio is left alone and the user is
        asked to toggle Bluetooth or power-cycle the device. The scan/connect
        loop keeps running, so a manual fix is picked up automatically.
        """
        if self._on_recover is None:
            self._on_status(
                "hardware",
                f"{self._connect_failures} consecutive connect failures -- "
                "toggle Bluetooth off/on or power-cycle the device.",
            )
            return
        now = time.perf_counter()
        if now - self._last_reset_t < _RESET_COOLDOWN_S:
            # Too soon after the previous reset; keep retrying for now.
            return
        if self._resets_done >= _MAX_AUTO_RESETS:
            self._on_status(
                "hardware",
                "Automatic Bluetooth reset did not restore the link. Please "
                "toggle Bluetooth off/on manually or power-cycle the device.",
            )
            return
        self._resets_done += 1
        self._last_reset_t = now
        self._on_status(
            "recovering",
            f"Link stuck; resetting Bluetooth "
            f"(attempt {self._resets_done}/{_MAX_AUTO_RESETS})...",
        )
        try:
            await asyncio.get_running_loop().run_in_executor(None, self._on_recover)
        except Exception as exc:  # best effort
            self._on_status("error", f"Recovery failed: {exc}")

    async def _connect_and_serve(self, device) -> None:
        """Connect with a fresh client, subscribe, and serve until disconnect."""
        self._on_status("connecting", f"Connecting to {protocol.DEVICE_NAME}...")
        self._disconnected_evt.clear()
        # Use a new client every time; reusing a closed WinRT object raises
        # WinError -2147483629.
        client = BleakClient(
            device,
            disconnected_callback=self._handle_disconnect,
            timeout=_CONNECT_TIMEOUT_S,
            # Force a new GATT discovery. Windows caches services per device,
            # and a stale cache from an earlier session makes the
            # characteristics "Unreachable".
            winrt={"use_cached_services": False},
        )
        try:
            await client.connect()
            self._client = client
            # Wait for the MTU, data length, 2M PHY and connection parameter
            # updates before subscribing, so streaming starts with 244-byte
            # payloads.
            await asyncio.sleep(_SETTLE_S)
            await client.start_notify(protocol.UUID_DATA_CHRC, self._notify_cb)
            self._notify_active = True
        except BaseException:
            # A failure during connect or service discovery (e.g. WinRT
            # "Unreachable") can leave a half-open link that the device treats
            # as connected until its supervision timeout. Disconnect explicitly
            # so the next attempt is not rejected.
            self._client = None
            self._notify_active = False
            try:
                await client.disconnect()
            except (BleakError, OSError):
                pass
            raise
        self._on_status("connected", f"Connected to {protocol.DEVICE_NAME}.")
        # A successful connection resets the failure counter and the reset
        # limit.
        self._connect_failures = 0
        self._resets_done = 0

        # If the link dropped during a recording, mark the gap and resume.
        if self._recording:
            self._on_gap()
            await client.write_gatt_char(
                protocol.UUID_STARTSTOP_CHRC, protocol.CMD_START, response=True
            )

        # Serve until the device disconnects or shutdown is requested.
        while not self._shutdown and client.is_connected:
            try:
                await asyncio.wait_for(self._disconnected_evt.wait(), timeout=1.0)
                break
            except asyncio.TimeoutError:
                continue

        if self._shutdown:
            await self._safe_disconnect()

    # Notification callback (BLE thread)

    def _notify_cb(self, _char, data: bytearray) -> None:
        t_recv = time.perf_counter()
        if not self._recording:
            return
        try:
            self._queue.put_nowait((bytes(data), t_recv))
        except queue.Full:
            # Queue full: drop the oldest item and count the drop.
            try:
                self._queue.get_nowait()
                self._queue.put_nowait((bytes(data), t_recv))
            except (queue.Empty, queue.Full):
                pass
            self._dropped += 1

    def _handle_disconnect(self, _client: BleakClient) -> None:
        self._notify_active = False
        self._on_status("disconnected", "Device disconnected.")
        if self._loop is not None and self._disconnected_evt is not None:
            self._loop.call_soon_threadsafe(self._disconnected_evt.set)

    async def _safe_disconnect(self) -> None:
        client = self._client
        self._client = None
        self._notify_active = False
        if client is not None:
            try:
                await client.disconnect()
            except (BleakError, OSError):
                pass

    # Thread-safe controls

    def _run_coro(self, coro) -> None:
        if self._loop is not None:
            asyncio.run_coroutine_threadsafe(coro, self._loop)

    def start_recording(self) -> None:
        """Send the start command. Safe to call from any thread."""
        self._recording = True
        self._run_coro(self._write_cmd(protocol.CMD_START))

    def stop_recording(self) -> None:
        """Send the stop command. Safe to call from any thread."""
        self._recording = False
        self._run_coro(self._write_cmd(protocol.CMD_STOP))

    async def _write_cmd(self, cmd: bytes) -> None:
        client = self._client
        if client is not None and client.is_connected:
            try:
                await client.write_gatt_char(
                    protocol.UUID_STARTSTOP_CHRC, cmd, response=True
                )
            except (BleakError, OSError) as exc:
                self._on_status("error", f"Failed to send command: {exc}")

    def shutdown(self) -> None:
        """Stop the worker and disconnect. Safe to call from any thread."""
        self._shutdown = True
        self._recording = False
        if self._loop is not None and self._disconnected_evt is not None:
            self._loop.call_soon_threadsafe(self._disconnected_evt.set)

    def wait_ready(self, timeout: float | None = None) -> bool:
        """Block until the BLE event loop is running."""
        return self._ready.wait(timeout)
