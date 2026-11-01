# PM660L LCDB upstream porting audit

Audit date: 2026-11-01

## Scope and source provenance

- Vendor reference repository: `sabarop/kernel_xiaomi_sdm660`
- Vendor reference commit: `aade098492360e5e72a29f8561b4d3864090df33`
- Driver: `drivers/regulator/qpnp-lcdb-regulator.c` (2,406 lines)
- PMIC description: `arch/arm64/boot/dts/vendor/qcom/pm660l.dtsi`
- Platina panel references:
  - `arch/arm64/boot/dts/vendor/qcom/xiaomi/mi/platina/platina-sdm660-mdss-panels.dtsi`
  - `arch/arm64/boot/dts/vendor/qcom/xiaomi/mi/platina/display/dsi-panel-tianma-fhd-nt36672a-video.dtsi`
- Target baseline: upstream Linux v7.3.1.

## Confirmed PM660L LCDB hardware description

The vendor PM660L DTS defines an LCDB function block at SPMI address
`0xec00`, size `0x100`, compatible `qcom,qpnp-lcdb-regulator`.
It exposes a short-circuit interrupt named `sc-irq` and a PMIC revision
phandle. Its child regulators are:

- LDO output: `lcdb_ldo_vreg`, 4.0-6.0 V.
- NCP output: `lcdb_ncp_vreg`, 4.0-6.0 V.

Platina's Tianma panel maps its positive `lab` rail to the LCDB LDO and
negative `ibb` rail to the LCDB NCP. The touch controller vendor node also
references these LCDB outputs. The upstream PM660L DTSI baseline does not
currently define this LCDB block, and upstream has no PM660L-specific LCDB
regulator driver.

## Upstream integration points

Linux v7.3.1 provides:

- `qcom_pmic_get(struct device *)` in
  `include/soc/qcom/qcom-spmi-pmic.h`.
- PMIC identity fields `subtype`, `major`, `minor` and `rev2`.
- `PM660L_SUBTYPE == 0x1a`.
- The PMIC core loads the PMIC revision before populating child devices.
- The standard PMIC child regmap access pattern is
  `dev_get_regmap(dev->parent, NULL)`.

The vendor's `pmic_rev_id->rev4` maps to the upstream PMIC `major` field,
because the upstream PMIC core reads PMIC_REV4 into `major`. The vendor
PM660L v2.0 threshold is revision 4 value `0x02`; the NCP short-circuit
workaround applies to PM660L revisions below that threshold.

## Register behavior that must be preserved

- PM660L protected writes require writing `0xa5` to base + `0xd0` before
  the protected register write.
- LCDB block base is `0xec00`.
- LDO and NCP outputs share the LCDB module-enable control.
- LDO and NCP have separate VREG_OK status registers/bits.
- The short-circuit IRQ path must preserve the vendor status-register
  pre-write/read sequence and the PM660L revision-specific workaround.
- PM660L boost voltage programming uses 50 mV steps and has a 6.25 V
  hardware ceiling; it must not be replaced with the PM8150L LCDB driver.
- The vendor default PM660L boost headroom is 200 mV.

## Confirmed vendor-driver defects to fix during the port

1. `GENAMSK(3, 2)` is a typo and must be `GENMASK(3, 2)`.
2. NCP current-limit encoding calculates a table index and then overwrites
   it with the raw mA value.
3. Boost current-limit encoding calculates the hardware code and then
   overwrites it with raw mA.
4. Pulse-skip threshold encoding calculates a code and then overwrites it
   with the raw threshold.
5. `is_lcdb_enabled()` returns a boolean and converts SPMI read errors into
   "disabled", allowing unsafe follow-on writes.
6. LDO and NCP regulator-core use counts are independent, but the hardware
   module enable is shared. Disabling either rail must not power down the
   other active rail.
7. The vendor `is_enabled` callbacks return one shared module flag rather
   than reading each output's own VREG_OK state.
8. Secure-mode regulator callbacks report success without performing the
   requested hardware transition, which can desynchronize regulator-core
   state. This behavior must be redesigned or rejected explicitly.
9. A short-circuit lockout path can make later enable calls return success
   without enabling hardware.
10. The probe path does not set platform drvdata although remove reads it.
11. Class registration is not unwound if later hardware initialization fails.
12. A `dev_err_probe()` call omits the `rc` format argument.
13. Voltage-ramp failure after module enable lacks a complete rollback path.
14. Probe-time hardware-already-enabled state must be reconciled with each
    output's actual status, not assumed to mean both outputs are enabled.

## Required complete patch scope

A complete PM660L LCDB upstream patch must include, as applicable:

1. A PM660L-specific regulator driver with explicit shared-module/per-output
   request state, error propagation, safe SC handling and cleanup.
2. Kconfig and Makefile integration.
3. A YAML devicetree binding for the PM660L LCDB block and its LDO/NCP
   regulator children.
4. The LCDB node in upstream `pm660l.dtsi`, with correct interrupt and
   PMIC parent relationship.
5. The Platina DTS enabling the LCDB block and defining only verified
   supplies/consumers.
6. Validation documentation and a full patch-series index.

Do not enable the Tianma panel or Novatek touch consumers until this driver
and its binding have been reviewed and the DTB passes build/schema checks.

## Validation status

This is a source audit, not a completed driver port. No PM660L LCDB patch has
been generated, compiled, hardware-tested or flashed. The next implementation
step is to derive the PM660L-specific regulator operations and state machine
from the vendor behavior while using the upstream PMIC/revid and regulator
framework APIs.
