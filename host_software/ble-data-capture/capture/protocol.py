"""BLE wire format of the APNEA_V1 quadruple-microphone device.

The values here follow the nRF52840 firmware (firmware/ in this repository) and have to
stay in sync with it.

* The device advertises the complete name ``APNEA_V1``. BT privacy is enabled,
  so the address can be a resolvable random address; we always match on the
  name instead of a fixed MAC.
* One custom GATT service with two characteristics:
    - DATA (notify): multiplexed sensor data.
    - START/STOP (read/write): write 0x01 to start streaming, 0x00 to stop.
* Each notification starts with a 4-byte little-endian uint32 modality header,
  followed by the payload:
    HEADER_MIC = 1  interleaved int16 microphone samples
    HEADER_PPG = 4  6-byte PPG record (not used by this host)
    2 (ECG) and 3 (ACC) are defined in the firmware but not streamed.
* MIC payload: little-endian int16, four channels interleaved
  (MIC1, MIC2, MIC3, MIC4, MIC1, ...), 4000 Hz per channel. The values are
  14-bit differential ADC codes stored in an int16.
* Packets carry no sequence number and no device timestamp. Data arrives in
  bursts of about 150 ms, so sample timing is rebuilt from the nominal sample
  rate starting at the first packet, and every disconnect is treated as a gap.
"""

from __future__ import annotations

import numpy as np

# Device name used for scanning
DEVICE_NAME: str = "APNEA_V1"

# GATT UUIDs
UUID_DATA_SVC: str = "86dd0000-b956-4fdc-9b9e-fa64a9b436f2"
UUID_DATA_CHRC: str = "86dd0001-b956-4fdc-9b9e-fa64a9b436f2"  # notify
UUID_STARTSTOP_CHRC: str = "86dd0002-b956-4fdc-9b9e-fa64a9b436f2"  # read/write

# Start/stop command bytes
CMD_START: bytes = b"\x01"
CMD_STOP: bytes = b"\x00"

# Packet header
HEADER_LEN: int = 4  # bytes, little-endian uint32 modality tag
HEADER_MIC: int = 1
HEADER_ECG: int = 2  # defined in firmware, not enabled
HEADER_ACC: int = 3  # defined in firmware, not enabled
HEADER_PPG: int = 4  # streamed, ignored here

# Microphone stream layout
SAMPLE_RATE: int = 4000            # Hz per channel (firmware TIMER1 at 4 kHz)
N_CHANNELS: int = 4                # MIC1..MIC4
SAMPLE_DTYPE = np.dtype("<i2")     # signed int16, little-endian
BYTES_PER_SAMPLE: int = 2
BYTES_PER_FRAME: int = N_CHANNELS * BYTES_PER_SAMPLE  # one sample per channel
BIT_DEPTH: int = 14                # differential: sign + 13 magnitude bits

CHANNEL_LABELS: tuple[str, ...] = ("MIC1", "MIC2", "MIC3", "MIC4")


def parse_header(payload: bytes) -> int:
    """Return the modality header of a notification.

    Args:
        payload: Full notification bytes (header + data).

    Returns:
        The modality tag as an integer, e.g. :data:`HEADER_MIC`.
    """
    return int.from_bytes(payload[:HEADER_LEN], byteorder="little", signed=False)
