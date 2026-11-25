# Host software

Host software for the wearable stethoscope described in *A Wearable Stethoscope for Source-Separated Body Acoustic Monitoring*.
It connects to the device (nRF52840, firmware in [`firmware/`](../firmware/)) over Bluetooth Low Energy, shows the four microphone channels live, and records them losslessly to FLAC with timing metadata.
It was used for all recordings in the study, including full-night sessions.

## Contents

```
ble-data-capture/
  main.py            capture app (GUI or headless)
  postprocess.py     offline DC removal, CSV / WAV / PNG export
  capture/
    protocol.py      BLE UUIDs, packet header, sample rate, channel layout
    ble.py           BLE worker: scan, connect, reconnect, notifications to a queue
    writer.py        de-interleaving, FLAC writer, drift log and metadata
    ringbuffer.py    buffer for the live plot
    plot.py          live view (pyqtgraph)
    btreset.py       Windows Bluetooth radio reset for link recovery
tests/               unit, integration and end-to-end tests (no hardware needed)
```

## 1. System requirements

- Windows 11 (tested). The automatic Bluetooth reset uses the Windows Runtime (`winrt-Windows.Devices.Radios`, installed on Windows only); the rest of the code uses cross-platform packages but has not been tested on macOS or Linux.
- Python 3.12 and [uv](https://docs.astral.sh/uv/) (tested with uv 0.11).
- Dependencies are pinned in `uv.lock`. Tested versions: Python 3.12.13, bleak 3.0.2, numpy 2.5.1, soundfile 0.14.0, PyQt6 6.11.0, pyqtgraph 0.14.0, matplotlib 3.11.2, pytest 9.1.
- Hardware: a Bluetooth LE adapter on the host and the wearable device running the firmware in `firmware/`. Recording requires the device; the tests and post-processing do not.

## 2. Installation

Clone or download the repository, then run from this folder (`host_software/`):

```bash
uv sync
```

`uv sync` creates `.venv` with the locked dependencies. Typical install time on a desktop computer is under one minute (about 5 s with a warm uv cache).

## 3. Demo

The test suite streams simulated device packets (same format and timing as the firmware: 150 ms bursts of 30-frame MIC packets plus PPG packets) through the writer and the capture controller, without hardware.

```bash
uv run pytest
```

Expected output: `13 passed`. Expected run time: about 10 s. The tests check that the FLAC file is bit-exact, that the measured sample rate is within 2 % of 4 kHz, and that gaps and metadata are recorded.

## 4. Instructions for use

**Record** (GUI). The app scans for the device named `APNEA_V1` and connects. Start is enabled once the device is ready; Stop saves the recording.

```bash
uv run python ble-data-capture/main.py
```

**Record without GUI** for a fixed duration, e.g. a 7 h night, and post-process at the end:

```bash
uv run python ble-data-capture/main.py --headless --duration 25200 --postprocess
```

Each recording is written to `data/<YYYY-MM-DD_THH-MM-SS>/` (change with `--out`):

| File | Contents |
| --- | --- |
| `recording.flac` | 4 channels, 4 kHz, int16, raw 14-bit ADC codes |
| `recording.json` | UTC start time, sample rate, channel order, measured rate, jitter, gaps, dropped packets, estimated loss, quality flag |
| `recording_drift.csv` | `(sample_index, host_perf_time)` every 0.1 s, for alignment with other instruments |

**Post-process** a recording (the raw FLAC is never modified):

```bash
# DC-removed FLAC (default)
uv run python ble-data-capture/postprocess.py data/<session>/recording.flac
# CSV for the analysis code; add --wav and/or --plot as needed
uv run python ble-data-capture/postprocess.py data/<session>/recording.flac --csv
```

`<session>_MIC_post.csv` has no header; column 0 is time in seconds and columns 1 to 4 are the DC-removed microphone channels. This is the input format of the signal processing and classification code ([`source_separation_classification/`](../source_separation_classification/)).

If the connection keeps failing although the device is advertising, the app resets the Bluetooth radio (at most twice, 25 s apart), which briefly disconnects all Bluetooth devices on the host. After that it asks for a manual Bluetooth toggle or a device power cycle.

## License

Released under the MIT License (see `LICENSE`).

## Other parts of the repository

- [`firmware/`](../firmware/): device firmware (nRF Connect SDK).
- [`source_separation_classification/`](../source_separation_classification/): source separation, segmentation and classification.
