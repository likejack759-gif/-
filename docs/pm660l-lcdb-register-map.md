# PM660L LCDB register and voltage encoding audit

Audit date: 2026-11-01
Vendor source: sabarop/kernel_xiaomi_sdm660
Vendor commit: aade098492360e5e72a29f8561b4d3864090df33
Driver: drivers/regulator/qpnp-lcdb-regulator.c
Target PMIC: PM660L, SPMI base 0xec00.

This document records source-derived encodings. It is not a datasheet
substitute; register write side effects and analog behavior still require
hardware validation.

## Register map used by the vendor driver

Offsets are relative to LCDB base 0xec00.

| Offset | Register | Evidence / purpose |
|---:|---|---|
| 0x08 | STS1 | Status window start |
| 0x0a | STS3 | LDO VREG_OK at bit 7 |
| 0x0b | STS4 | NCP VREG_OK at bit 7 |
| 0x10 | INT_RT_STATUS | overall VREG_OK bit 0; SC error bit 1 |
| 0x41 | BST output voltage | PM660L 5-bit selector, bits 4:0 |
| 0x45 | Module ready | bit 7 |
| 0x46 | Enable control 1 | module enable bit 7; HWEN ready bit 6 |
| 0x47 | BST pull-down | disable pull-down bit 1; strength bit 0 |
| 0x4b | BST current limit | enable bit 7; selector bits 2:0 |
| 0x50 | Pulse-skip control | enable bit 7; PM660L threshold selector bits 1:0 |
| 0x53 | RDS(on) management | FET sizing |
| 0x5f | LCDB soft start | PM660L soft-start field |
| 0x60 | Misc control | auto GM bit 4; touch wake bit 3; SCP disable bit 0 |
| 0x66 | Power-up/down control | protected write in PM660L settings restore |
| 0x71 | LDO output voltage | selector bits 4:0 |
| 0x75 | LDO VREG_OK debounce |
| 0x77 | LDO pull-down |
| 0x79 | LDO force pull-down |
| 0x7b/0x7c | LDO current limit | enable/selector and companion field |
| 0x7f | LDO soft start |
| 0x81 | NCP output voltage | selector bits 4:0 |
| 0x85 | NCP VREG_OK debounce |
| 0x87 | NCP pull-down |
| 0x8b/0x8c | NCP current limit | enable/selector and companion field |
| 0x8f | NCP soft start |
| 0xd0 | Secure access key | write 0xa5 before protected 0x66 write |

The vendor IRQ handler explicitly writes two zero bytes beginning at STS3
before reading STS3/STS4. Preserve this pre-write/read sequence; do not
replace it with direct reads until hardware semantics are independently
verified.

The PM660L settings table marks only PWRUP_PWRDN_CTL (0x66) as secure-access
within the PM660L saved/restored register list. The secure-write helper
writes 0xa5 to base+0xd0 before writing the requested register. Do not infer
that every LCDB register requires this key.

## Output voltage selector

The output selector is a piecewise mapping over all 32 values:

- selector 0..9: 4,000,000 uV + selector * 100,000 uV;
- selector 10..31: 4,950,000 uV + (selector - 10) * 50,000 uV.

Boundary values:
- selector 0 = 4.00 V;
- selector 9 = 4.90 V;
- selector 10 = 4.95 V;
- selector 31 = 6.00 V.

There is no supported selector for 4.91..4.94 V. A regulator-core interval
that contains no listed voltage must fail with -EINVAL; never silently
round above max_uV.

The vendor set_voltage() uses min_uV/1000 and ignores max_uV. The upstream
implementation must instead map the caller's full [min_uV, max_uV] interval
to the lowest representable voltage inside it.

## Boost selector

For PM660L the boost selector uses bits 4:0:

- minimum = 4,700,000 uV;
- step = 50,000 uV;
- 32 selectors;
- maximum = 6,250,000 uV.

The boost target in the vendor algorithm is:
max(LDO voltage, abs(NCP voltage)) + headroom,
with a 200,000 uV default PM660L headroom and a 4.7 V minimum.

The PM660L hardware ceiling is 6.25 V. The driver must reject an impossible
requested combination rather than silently claiming the required headroom
was maintained after clipping.

## Revision handling

The vendor revision header defines PM660L v2.0 revision-4 value as 0x02.
The LCDB driver enables NCP short-circuit-protection disable workaround when
PM660L rev4 < 0x02.

In upstream Linux v7.3.1, qcom_pmic_get() returns struct qcom_spmi_pmic,
whose major field is populated from PMIC_REV4. The port should use
pmic->major < 0x02 for this vendor-equivalent revision test after verifying
PM660L subtype 0x1a.

## Shared module and output status

- Both outputs share MODULE_EN bit 7 at offset 0x46.
- LDO physical VREG_OK is STS3 bit 7.
- NCP physical VREG_OK is STS4 bit 7.
- Overall LCDB VREG_OK is INT_RT_STATUS bit 0.
- SC error is INT_RT_STATUS bit 1.

The software request mask, shared module-enable bit and two physical output
status bits are different pieces of state. They must not be conflated.

## PM660L-specific NCP short-circuit workaround

For revisions below v2.0, the vendor enable workaround:
1. writes MODULE_EN to the enable-control register;
2. writes zero to that register to disable the module;
3. waits 10–10.1 ms for the boost MID pin to discharge;
4. sets DIS_SCP in MISC_CTL;
5. waits 1–1.1 ms;
6. clears DIS_SCP;
7. waits 1–1.1 ms.

This sequence is separate from the normal final module-enable operation.
The upstream implementation must preserve ordering and check every write.
The IRQ recovery path must not proceed to re-enable after a failed shutdown.

## Source defects relevant to selector implementation

The vendor NCP current-limit, BST current-limit and pulse-skip threshold
paths compute encoded values and then overwrite them with raw milliamp
values. Those optional controls are excluded from the first implementation
until their full tables, valid values and encoding behavior are separately
verified.

## Initial helper implementation

drivers/regulator/qcom-pm660l-lcdb-internal.h contains pure selector and
boost-target helpers derived from the mappings above.
tests/pm660l-lcdb-selector-test.c exercises all 32 output selectors,
piecewise boundaries, invalid intervals, boost endpoints and headroom
limits.

The host test is a selector fixture test only. It does not prove kernel
compilation, regmap behavior, regulator-core integration or hardware safety.
