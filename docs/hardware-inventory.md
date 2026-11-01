# Xiaomi Mi 8 Lite (platina) hardware inventory

This document separates runtime observations from items that still require
source-level or electrical verification. It is not a claim that the board is
already fully supported by mainline Linux.

## Platform identity

- Device: Xiaomi Mi 8 Lite
- Board codename: `platina`
- SoC: Qualcomm Snapdragon 660 / SDM660
- Architecture: ARM64
- Observed Android environment: Android 10, SDK 29
- Observed vendor kernel: `4.4.192-perf-g9a24ce6`
- Runtime device-tree compatible strings include
  `qcom,sdm660-mtp`, `qcom,sdm660` and `qcom,mtp`.
- Runtime model string identifies SDM660 PM660 + PM660L and Platina.

## Display

Observed active panel:

- Tianma FHD NT36672A video-mode DSI panel
- DSI0, single DSI link, four data lanes
- Resolution: 1080 x 2280
- Reported mode: 60 Hz
- Backlight: WLED, reported range 1..4095

The vendor panel command sequence and reset/power timing are device-specific.
The upstream NT36672A panel driver is not assumed to be electrically or
protocol-compatible. Do not enable a panel node until a Platina-specific
driver variant and its regulator bindings have been reviewed.

## Touchscreen

Observed active device:

- I2C device: bus 4, address 0x62
- Driver/device name: `NVT-ts`
- OF compatible: `novatek,NVT-ts`
- Controller node: `/soc/i2c@c178000/novatek@62`

Synaptics TCM at address 0x20 and FocalTech at 0x38 were also observed as
alternative device entries. They must not all be enabled as if they were
simultaneously fitted. The active NVT firmware image and update behavior
require separate verification; do not enable automatic controller flashing
without the exact matching firmware.

## Power and regulators

The vendor device tree associates the Tianma panel and NVT touchscreen with
PM660/PM660L supplies, including PM660L LCDB LDO/NCP outputs. The upstream
baseline does not yet provide a verified PM660L LCDB implementation for this
board. Do not substitute unrelated PMIC LCDB drivers or invent regulator
nodes.

## GPU and multimedia

- GPU family: Adreno 512.
- The SoC display pipeline is MDP5, not DPU3.
- Camera, audio, Wi-Fi, Bluetooth, charging, modem and other peripherals
  require individual driver, firmware, regulator, clock, GPIO and interrupt
  audits before being enabled.

## Safety constraints

- Preserve the original Android 10 boot image and recovery path.
- Do not write to modem, modemst, fsg, fsc or persist partitions.
- Do not flash a DTB/DTBO until it has passed source build, device-tree
  validation and a recoverable boot test.
