"""Bluetooth radio reset on Windows, used to recover a stale BLE session.

The APNEA_V1 accepts a single connection. When a host session ends abnormally
(for example the process is killed), the Windows BLE stack can keep a
half-open GATT session that blocks new connections: the device shows up in
scans but ``client.connect()`` times out. Turning the Bluetooth radio off and
on clears it, which is what this module does.

Note that this resets the whole radio, so every Bluetooth device on the host
(mouse, keyboard, headphones) is briefly disconnected.
"""

from __future__ import annotations

import asyncio

# The radio has to stay off long enough for Windows to release the stale
# session; a shorter toggle does not clear it.
_OFF_SETTLE_S = 3.5
_ON_SETTLE_S = 3.0


async def _set_bluetooth(state_on: bool) -> bool:
    from winrt.windows.devices.radios import (
        Radio,
        RadioAccessStatus,
        RadioKind,
        RadioState,
    )

    access = await Radio.request_access_async()
    if access != RadioAccessStatus.ALLOWED:
        # Without access, set_state_async silently does nothing, so raise
        # instead of reporting a reset that did not happen.
        raise PermissionError(
            f"Radio access not allowed ({access!r}); cannot toggle Bluetooth. "
            "Enable it under Settings > Privacy & security > Radios."
        )
    radios = await Radio.get_radios_async()
    target = RadioState.ON if state_on else RadioState.OFF
    found = False
    for r in radios:
        if r.kind == RadioKind.BLUETOOTH:
            found = True
            await r.set_state_async(target)
    return found


async def reset_bluetooth_async() -> bool:
    """Toggle the Bluetooth radio off then on. Returns True if a BT radio was found."""
    ok = await _set_bluetooth(False)
    if not ok:
        return False
    await asyncio.sleep(_OFF_SETTLE_S)
    await _set_bluetooth(True)
    await asyncio.sleep(_ON_SETTLE_S)
    return True


def reset_bluetooth() -> bool:
    """Synchronous wrapper around :func:`reset_bluetooth_async`."""
    return asyncio.run(reset_bluetooth_async())


if __name__ == "__main__":
    print("Resetting Bluetooth radio (this disconnects all BT devices)...", flush=True)
    ok = reset_bluetooth()
    print("done" if ok else "no Bluetooth radio found", flush=True)
