"""BLE microphone capture for the APNEA_V1 device.

Modules:
    protocol   -- wire format constants shared with the firmware.
    ringbuffer -- thread-safe circular buffer for the live plot.
    ble        -- BLE connection worker (bleak).
    writer     -- streaming FLAC writer and timing metadata.
    plot       -- pyqtgraph live view.
    btreset    -- Windows Bluetooth radio reset used for link recovery.
"""

from __future__ import annotations

__version__ = "1.0.0"
