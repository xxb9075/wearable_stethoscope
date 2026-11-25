# wearable_stethoscope

Source code for the article *A Wearable Stethoscope for Source-Separated Body Acoustic Monitoring*.

The wearable stethoscope combines a stretchable patch with four MEMS microphones and a wireless data acquisition module (nRF52840, Bluetooth Low Energy).
The module samples the four microphone channels at 4 kHz and streams them to a computer, which records them for analysis.
The analysis separates the four-channel recording into respiratory and cardiac components with independent component analysis (AMICA), segments the respiratory signal into breath phases and classifies inspiration, expiration and pause with a ResNet-34 network.

## Contents

| Folder | Contents | Language / toolchain | Used for |
| --- | --- | --- | --- |
| [`firmware/`](firmware/) | Firmware of the acquisition module: SAADC sampling, PPG sensor hub, packetization, BLE streaming | C, nRF Connect SDK v1.7.1 (Zephyr) | all recordings |
| [`host_software/`](host_software/) | BLE host software: live view, lossless FLAC recording with timing metadata, CSV export | Python 3.12, uv | all recordings |
| [`source_separation_classification/`](source_separation_classification/) | Source separation, signal conditioning, breath-phase segmentation and phase classification | MATLAB R2024a with EEGLAB/AMICA; Python 3.12, uv, PyTorch | Fig. 3 and Fig. 4 |

Data flow:

```
device (firmware/)  --BLE-->  host_software/  -->  <session>_MIC_post.csv (4 channels, 4 kHz)
  -->  source_separation_classification/preprocessing/   MATLAB: AMICA separation, conditioning, segmentation
  -->  source_separation_classification/src/             Python: framesets, ResNet-34 training and evaluation
```

Each folder is a self-contained project with its own README covering system requirements, installation, a demo with expected output and run time, and instructions for use.

## System requirements

- Host software and classification: Windows 11 (tested), Python 3.12 and [uv](https://docs.astral.sh/uv/) 0.11. Exact package versions are pinned in each folder's `uv.lock`.
- Source separation: MATLAB R2024a with the Signal Processing, Wavelet, Audio, and Statistics and Machine Learning Toolboxes, and EEGLAB 2025.1.0 with the AMICA plugin 1.7.
- Firmware: nRF Connect SDK v1.7.1 (Zephyr 2.6.99-ncs1-1) with GNU Arm Embedded 9-2019-q4-major, nRF Command Line Tools and SEGGER J-Link.
- Hardware: recording needs the wearable device (or an nRF52840 DK for firmware development) and a Bluetooth LE adapter. The demos of the host software and the classification code run without hardware.

## Installation and demo

Clone or download this repository, then follow the README of each folder. In short:

```bash
# Host software: install and run the test suite on simulated device packets (about 10 s)
cd host_software
uv sync
uv run pytest

# Classification: install, then run the demo on simulated data (about 2 min on a desktop CPU)
cd ../source_separation_classification
uv sync
# download the RespireNet checkpoint and run the demo commands in source_separation_classification/README.md
```

The firmware is built and flashed with `west` from `firmware/`; see [`firmware/README.md`](firmware/README.md).

## Data

The study recordings are not part of this repository; see the Data availability statement of the article.
The classification demo generates a small simulated dataset.

## License

Released under the MIT License (see [`LICENSE`](LICENSE)).
Third-party code in `firmware/` remains under its original licenses: the Apache License 2.0 for files based on Zephyr and Nordic Semiconductor examples, and the Beerware notice of the SparkFun Bio Sensor Hub library in `firmware/drivers/max32664/max32664.c` (see [`firmware/LICENSE`](firmware/LICENSE)).
The classification code is adapted from [RespireNet](https://github.com/microsoft/RespireNet) (MIT License).
