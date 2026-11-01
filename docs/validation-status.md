# Platina device-tree validation status

## Baseline

- Intended kernel baseline: upstream Linux v7.3.1.
- SoC description: `sdm660.dtsi`, which includes `sdm630.dtsi`.
- PMIC descriptions: `pm660.dtsi` and `pm660l.dtsi`.
- Board DTS: conservative initial skeleton.

## Source audit completed

The following points were checked against the upstream v7.3.1 Qualcomm DTS sources:

- `sdm660.dtsi` includes `sdm630.dtsi`.
- `blsp1_uart2` is defined in the inherited SoC DTSI and is disabled by
  default; the board file explicitly enables it.
- Upstream Xiaomi Redmi Note 7 (lavender) uses the same SoC/PMIC include
  sequence and the same UART2 alias/console pattern. This validates the
  syntax pattern, not the Platina-specific UART pin routing or boot console.
- PM660 and PM660L DTSI fragments extend the inherited SPMI bus with their
  respective PMIC nodes.
- The upstream Qualcomm Makefile contains the SDM660 lavender target but no
  Platina target.

## Included in this initial change

- Board model and compatible strings.
- Serial console alias and stdout path.
- BLSP1 UART2 enabled as a provisional console path.
- A three-patch initial series, ordered as binding, DTS source, then Makefile
  DTB target. The DTS source is introduced before its build target.

## Deliberately not enabled

Display, touchscreen, cameras, audio, GPU performance operating points,
Wi-Fi, Bluetooth, charging, battery, USB-C, fingerprint, sensors, modem and
other board peripherals remain deferred until their upstream bindings,
drivers, firmware and hardware dependencies have been verified.

## Validation state

- Branch: `dev/platina-device-tree-v1`.
- The three patch files are stored in dependency order, with an aggregate
  complete-series patch and an application index.
- `git apply --check` and `git apply` passed sequentially against a local
  fixture containing the exact upstream v7.3.1 binding and Makefile context.
- The fixture test does not replace validation against a complete kernel tree.
- The DTS has **not** yet been compiled with the kernel's `dtc` build.
- `dtbs_check` has **not** yet been run.
- No DTB/DTBO has been boot-tested on hardware.
- No kernel image has been built or flashed as part of this change.

## Next validation steps

1. Apply the complete three-patch series to an exact Linux v7.3.1 source tree.
2. Run `make dtbs_check` for the Qualcomm binding and build
   `sdm660-xiaomi-platina.dtb` with the kernel build system.
3. Resolve all include, label, compiler and schema errors.
4. Compare inherited SoC/PMIC nodes with the device's runtime tree.
5. Add one hardware subsystem at a time, with its driver and power sequence.
6. Keep the original boot image available and test using a recoverable,
   non-destructive boot method before considering any flash operation.
