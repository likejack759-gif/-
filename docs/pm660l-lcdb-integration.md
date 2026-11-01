# PM660L LCDB kernel integration notes

Target base: Linux v7.3.1 commit
79a907609d53d5cd645b42c11550d295d43abf99.

## Kconfig fragment

Add near the Qualcomm regulator entries in drivers/regulator/Kconfig:

```Kconfig
config REGULATOR_QCOM_PM660L_LCDB
	tristate "Qualcomm PM660L LCDB regulator"
	depends on MFD_SPMI_PMIC
	depends on OF
	help
	  This driver supports the PM660L LCDB LDO and NCP outputs. The
	  outputs share a boost converter and module-enable control.
```

MFD_SPMI_PMIC provides qcom_pmic_get() and the parent SPMI regmap.

## Makefile fragment

Add to drivers/regulator/Makefile:

```make
obj-$(CONFIG_REGULATOR_QCOM_PM660L_LCDB) += qcom-pm660l-lcdb-regulator.o
```

The fragments are also stored under patches/pm660l/.

## Device-tree node shape

The binding is drafted at:
Documentation/devicetree/bindings/regulator/qcom,pm660l-lcdb-regulator.yaml

The node must be a child of the PM660L SPMI device at USID 3:

- compatible = "qcom,pm660l-lcdb-regulator";
- reg = <0xec00> (one address cell in the target PM660L DTSI);
- interrupts = <0x3 0xec 0x1 IRQ_TYPE_EDGE_RISING>;
- interrupt-names = "sc-irq";
- child nodes named "ldo" and "ncp".

The full vendor DTS was checked with its parent-node boundaries. The LCDB
node is nested below qcom,pm660l@3, so its SID 3 interrupt specifier matches
the parent SPMI device. The earlier apparent SID 2/3 mismatch was a false
alarm caused by reading the local excerpt without its parent scope.

The driver requires sc-irq for PMIC revisions that use IRQ-based short-
circuit recovery (PMIC REV4 >= 0x02). Older revisions use the PM660L NCP
SCP workaround and do not request this IRQ.

Each rail child is passed to the regulator core as its OF node. Standard
regulator constraints such as regulator-name, regulator-min-microvolt,
regulator-max-microvolt, regulator-boot-on and regulator-always-on may be
provided, subject to validation against the regulator core's boot handoff
semantics.

## Integration status

1. The PM660L-specific binding draft exists but has not passed
   dt_binding_check.
2. A PM660L USID 3 node fragment exists at
   patches/pm660l/pm660l-usid3-node.fragment. It is not yet applied to the
   complete target pm660l.dtsi.
3. The Platina board DTS does not yet enable the LCDB node or add panel/
   touch regulator consumers. Keep those consumers disabled until the
   driver passes full-tree validation.
4. The driver has not been compiled against the complete target tree.
   Sparse, dt_binding_check, dtbs_check and KUnit/regmap fault injection
   remain outstanding.
5. The first implementation omits current-limit controls, pulse-skip
   thresholds, soft-start configuration, force-pulldown/touch-wake policy,
   vendor sysfs/class controls and regulator fault notifier propagation.
   These must be added only after their register semantics and ABI are
   independently reviewed.

## First-pass behavior

- Registers two regulator devices: LDO and NCP.
- Models shared module enable with independent Linux consumer request bits.
- Reports is_enabled from each output's physical VREG_OK status.
- Reads the two status bytes using the vendor-required STS3/STS4 pre-write.
- Performs the vendor PM660L module on/off enable pulse on every revision;
  REV4 < 0x02 additionally uses the 10 ms discharge and DIS_SCP toggle.
- For PMIC revisions using IRQ recovery, confirms SC events using the
  vendor touch-wake 300 us or normal 2 ms blanking path.
- Bounds repeated recovery attempts and latches a fault after persistent
  failures.
- Coordinates output and boost voltage writes: raise boost first; lower
  boost only after lowering the output.
- Attempts to roll back uncertain register transactions; if rollback fails,
  the provider is fault-latched and further changes are refused.
- Does not write any modem, persist, boot, dtbo or other storage partition.

This is an implementation checkpoint, not a production-ready or
hardware-validated driver.
