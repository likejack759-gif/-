# Platina device-tree validation status

## Baseline

- Intended kernel baseline: upstream Linux v7.3.1.
- SoC description: SDM660, including inherited SDM630 definitions.
- PMIC descriptions: PM660 and PM660L.
- Board DTS: initial conservative skeleton only.

## Included in this initial change

- Board model and compatible strings.
- Serial console alias and stdout path.
- BLSP1 UART2 enabled for an initial console path.

## Deliberately not enabled

Display, touchscreen, cameras, audio, GPU performance operating points,
Wi-Fi, Bluetooth, charging, battery, USB-C, fingerprint, sensors, modem and
other board peripherals remain deferred until their upstream bindings,
drivers and hardware dependencies have been verified.

## Validation state

- Repository branch created: `dev/platina-device-tree-v1`.
- DTS source has **not** yet been compiled with `dtc`.
- `dtbs_check` has **not** yet been run.
- No DTB/DTBO has been boot-tested on hardware.
- No kernel image has been built or flashed as part of this change.

## Next validation steps

1. Build the DTS against the exact Linux v7.3.1 source tree.
2. Resolve all include, label and compiler errors.
3. Run `make dtbs_check` with the relevant Qualcomm schemas.
4. Review the generated DTB and compare inherited SoC/PMIC nodes with the
   device's runtime tree.
5. Add one hardware subsystem at a time, with its driver and power sequence.
6. Keep the original boot image available and test using a recoverable,
   non-destructive boot method before considering any flash operation.
