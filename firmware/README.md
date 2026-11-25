# Firmware

Firmware for the wireless data acquisition module of the wearable stethoscope described in *A Wearable Stethoscope for Source-Separated Body Acoustic Monitoring*.
It runs on the nRF52840 SoC under Zephyr RTOS (nRF Connect SDK), samples the four microphone channels and the PPG sensor hub, and streams the data to a host over Bluetooth Low Energy.

## Overview

- **Acquisition** (`drivers/nordic_adc`): SAADC sampling of four differential microphone channels at 4 kHz per channel, 14-bit resolution. TIMER1 triggers each conversion through PPI, and EasyDMA writes to a double buffer.
- **Microphone channels** (`app/app_mic`): de-interleaving and buffering of the ADC data.
- **PPG** (`drivers/max32664`, `app/app_ppg`): heart rate and SpO2 from the MAX32664 sensor hub (MAX30101) over I2C.
- **Packetization** (`app/app_data`): each BLE payload starts with a 4-byte modality header (1 = microphone, 4 = PPG) followed by the data.
- **BLE** (`app/data_ble`, `src/main.c`): peripheral named `APNEA_V1`, custom service `86dd0000-b956-4fdc-9b9e-fa64a9b436f2` with a notify characteristic for data (`...0001...`) and a start/stop characteristic (`...0002...`, write 0x01 to start, 0x00 to stop). 2M PHY, 251-byte data length and a 247-byte MTU are requested on connection.

The accelerometer driver (`drivers/kx132`) is included but disabled in `prj.conf`.

```
boards/arm/apnea_v2/     board definition of the stethoscope PCB (current revision)
boards/arm/apnea_v1/     earlier PCB revision
boards/arm/nrf52840dk_nrf52840/   devicetree overlay for the nRF52840 DK
drivers/                 SAADC, MAX32664 and KX132 drivers
app/                     microphone, PPG, packetization and BLE modules
include/                 headers
src/main.c               BLE setup and application start
prj.conf                 Kconfig settings
```

## 1. System requirements

- nRF Connect SDK **v1.7.1** (Zephyr 2.6.99-ncs1-1) and its toolchain (GNU Arm Embedded 9-2019-q4-major, CMake >= 3.20, west). The firmware used in the study was built with these versions. Newer SDK versions are not supported without porting (header paths and board definitions changed in later Zephyr releases).
- Tested on Windows 11 with the nRF Connect for VS Code extension and the nRF Connect SDK Toolchain Manager.
- nRF Command Line Tools (nrfjprog) and SEGGER J-Link software for flashing.
- Hardware: the stethoscope acquisition board (`apnea_v2`) with a SEGGER J-Link probe, or an nRF52840 DK (PCA10056) for development. Logs are printed over SEGGER RTT.

## 2. Installation

1. Install nRF Connect SDK v1.7.1 with the Toolchain Manager in nRF Connect for Desktop (recommended; it installs the SDK together with the matching toolchain).
   Alternatively, check out the SDK with west and install the toolchain listed above and the Python requirements of the SDK (`zephyr/scripts/requirements.txt`) by hand, following the nRF Connect SDK v1.7.1 "Installing manually" guide:
   ```bash
   west init -m https://github.com/nrfconnect/sdk-nrf --mr v1.7.1 ncs
   cd ncs && west update
   ```
2. Clone or download the repository to a folder outside the SDK. The build commands below are run from this folder (`firmware/`).

Typical install time is 30 to 60 minutes, mostly for downloading the SDK and toolchain.

## 3. Demo

Build and flash on an nRF52840 DK, from a shell where the v1.7.1 toolchain and `ZEPHYR_BASE` are set:

```bash
west build -b nrf52840dk_nrf52840 -d build_nrf52840dk -- -DDTC_OVERLAY_FILE=boards/arm/nrf52840dk_nrf52840/nrf52840dk_nrf52840.overlay
west flash -d build_nrf52840dk
```

Expected output: the RTT log shows `Bluetooth initialized`, `Advertising successfully started` and `Advertising started`, and the board advertises as `APNEA_V1`. To record, install the host software in [`host_software/`](../host_software/) and run `uv run python ble-data-capture/main.py` there; it connects to `APNEA_V1`, and Start begins streaming. The log then shows the MTU, data length and PHY updates and the host receives 4-channel microphone data at 4 kHz. Without microphones attached the DK streams ADC noise. Without the MAX32664 sensor hub the PPG packets are still sent, but their values are not valid (the driver does not stop when the hub does not respond).
A clean build takes about 1 to 2 minutes.

## 4. Instructions for use

Build for the stethoscope board and flash with a J-Link probe:

```bash
west build -b apnea_v2 -d build_apnea_v2
west flash -d build_apnea_v2
```

The board folder is added to `BOARD_ROOT` by `CMakeLists.txt`. In VS Code, add a build configuration in the nRF Connect extension with board `apnea_v2` (or `nrf52840dk_nrf52840` plus the overlay above) and configuration `prj.conf`.

Settings that may be changed in `prj.conf`: device name (`CONFIG_BT_DEVICE_NAME`) and the preferred connection parameters. The packetization module (`app_data`) always uses both the microphone and the PPG module, so `CONFIG_APP_MIC`, `CONFIG_APP_PPG` and `CONFIG_MAX32664` have to stay enabled; `CONFIG_KX132` adds the accelerometer driver. The sampling rate (4 kHz), oversampling and ADC resolution are set in `MicEcgAdcInit()` in `app/app_mic/app_mic.c`, which overrides the defaults in `drivers/nordic_adc/nordic_adc.c`. If any of the BLE settings or the packet format are changed, the host software has to be changed to match.

## License

Released under the MIT License (see `LICENSE`). Files with their own SPDX header (`src/main.c`, board files, `CMakeLists.txt`, the MAX32664 binding), which are based on Zephyr and Nordic Semiconductor examples, remain under the Apache License 2.0 (`LICENSE-APACHE-2.0`).
The MAX32664 driver (`drivers/max32664/max32664.c`) is ported from the SparkFun Bio Sensor Hub Arduino Library (SparkFun Electronics), released under the Beerware license; the notice is kept in the file.

## Other parts of the repository

- [`host_software/`](../host_software/): BLE host software for recording.
- [`source_separation_classification/`](../source_separation_classification/): source separation and respiratory phase classification.
