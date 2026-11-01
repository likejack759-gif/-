# PM660L LCDB kernel integration notes

Target base: Linux v7.3.1 commit
79a907609d53d5cd645b42c11550d295d43abf99.

## Kconfig fragment

Add near the Qualcomm regulator entries in drivers/regulator/Kconfig:

```Kconfig
config REGULATOR_QCOM_PM660L_LCDB
	tristate "Qualcomm PM660L LCDB regulator driver"
	depends on MFD_SPMI_PMIC
	depends on OF
	help
	  Say y here to enable the PM660L LCDB LDO and NCP regulators.
	  The two outputs share a boost converter and module-enable bit.

	  Say M here if you want to build the driver as a module. The module
	  will be called "qcom-pm660l-lcdb".
```

MFD_SPMI_PMIC is the target tree's Qualcomm SPMI PMIC parent driver. It
provides qcom_pmic_get() and the parent regmap used by this driver.

## Makefile fragment

Add to drivers/regulator/Makefile:

```make
obj-$(CONFIG_REGULATOR_QCOM_PM660L_LCDB) += qcom-pm660l-lcdb.o
```

## Driver node shape expected by the current implementation

The current driver expects the LCDB platform node to provide:
- compatible = "qcom,pm660l-lcdb";
- reg = <0xec00 0x100> (the driver currently reads the first reg cell);
- child nodes named "ldo" and "ncp";
- optional interrupt-names = "sc-irq" and a corresponding interrupt.

Each rail child is passed to the regulator core as its OF node, so standard
regulator constraints such as regulator-name, regulator-min-microvolt,
regulator-max-microvolt, regulator-boot-on and regulator-always-on should
be used. The binding must define and validate this shape before a DTS node
is added.

## Explicit integration blockers

1. The vendor DTS nests the LCDB block under PM660L USID 2 but encodes the
   SC interrupt specifier with SID 3. The target binding defines the first
   interrupt cell as the SPMI slave ID. Do not add an interrupt specifier
   until the runtime/arbiter mapping is verified.
2. The driver currently treats the SC interrupt as optional. Without it,
   regulator enable/voltage control works but automatic SC recovery is not
   available. Do not enable the board node for production until the correct
   interrupt route is established.
3. The current driver is not yet compiled against the complete target tree.
   API review has been done against v7.3.1 headers, but build, sparse,
   dt_binding_check, dtbs_check and KUnit/regmap fault injection remain.
4. The driver intentionally does not implement touch-wake sysfs/class
   controls, current-limit controls, soft-start controls, or vendor debug
   interfaces in this first pass. They must be added only after their
   register semantics and ABI are independently reviewed.

## First-pass behavior and limitations

- Registers two regulator devices: LDO and NCP.
- Models shared module enable with independent Linux consumer request bits.
- Reports is_enabled from the respective physical VREG_OK status bit.
- Reads the two status bytes using the vendor-required STS3/STS4 pre-write.
- Uses PM660L revision-4 < 0x02 NCP SCP workaround.
- Preserves the vendor behavior of not requesting the SC IRQ on revisions
  requiring that workaround.
- Coordinates output and boost voltage writes: raise boost first; lower
  boost only after the output has been lowered.
- On an output-write failure after raising boost, attempts to restore the
  old boost selector. A failed rollback marks the hardware state unknown.
- A failed module shutdown during SC handling prevents automatic restart.
- Does not write any modem, persist, boot, dtbo or other storage partition.

This is an implementation checkpoint, not a production-ready or
hardware-validated driver.
