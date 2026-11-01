# PM660L LCDB driver implementation review

Date: 2026-11-01
Target Linux baseline: v7.3.1
Target commit: 79a907609d53d5cd645b42c11550d295d43abf99

## Current implementation

Source:
- drivers/regulator/qcom-pm660l-lcdb-regulator.c
- drivers/regulator/qcom-pm660l-lcdb-internal.h

Integration fragments:
- patches/pm660l/Kconfig.fragment
- patches/pm660l/Makefile.fragment

The first driver implementation provides:
- PM660L subtype validation through qcom_pmic_get().
- SPMI parent regmap access and LCDB base parsing.
- Separate regulator devices for LDO and NCP child nodes.
- Shared module-enable ownership tracked independently from physical
  LDO/NCP VREG_OK status.
- PM660L v1.x NCP SCP workaround gated by PMIC REV4 (major < 0x02).
- Per-output selector mapping and coordinated shared boost voltage.
- Boost-before-output sequencing when raising the required boost voltage.
- Output-before-boost sequencing when lowering boost voltage.
- Best-effort rollback after failed writes; ambiguous rollback state
  latches the provider faulted.
- PM660L status-register pre-write/read sequence.
- Short-circuit IRQ debounce matching downstream touch-wake and normal
  display-blanking paths, bounded recovery, and shutdown on repeated faults.

## Deliberately excluded from this first implementation

- NCP/BST/LDO current-limit programming.
- Pulse-skip threshold programming.
- Soft-start configuration.
- Force-pulldown and touch-wake policy configuration.
- Vendor sysfs/class interfaces.
- Secure PWRUP_PWRDN_CTL settings save/restore (register 0x66).
- Regulator fault notifier propagation.

These features must be added only after their DT ABI, exact encodings and
failure semantics have separate evidence and tests. The source-derived
current-limit code has known encoding defects and is not safe to port as-is.

## Outstanding blockers before upstream-quality completion

1. **SPMI IRQ SID**: vendor PM660L DTS places LCDB under USID 2 but encodes
   interrupt SID 3. This routing must be resolved before enabling the IRQ
   through a final binding and board DTS.
2. **Binding**: no YAML binding exists yet for qcom,pm660l-lcdb-regulator.
   Define the PM660L-specific compatible and two regulator child nodes.
3. **Kernel integration**: Kconfig/Makefile fragments are prepared but have
   not been applied to a complete Linux v7.3.1 source checkout.
4. **Compile**: no complete-tree object build or modpost has been run.
5. **Fault injection**: no regmap fault-injection tests exist yet. The host
   helper fixture does not test the driver state machine.
6. **Hardware**: no PM660L hardware execution, voltage measurement, display
   sequencing or short-circuit test has been performed.
7. **PMIC boot state**: verify whether MODULE_RDY and the bootloader-enabled
   handoff behavior are correct on the actual Platina PM660L revision.
8. **Voltage transaction**: confirm the exact boost/output update sequencing
   and headroom behavior on hardware; rollback code is conservative but
   cannot prove analog transients from software inspection alone.

## Review cautions

- Do not add the LCDB node to platina DTS until the SID discrepancy is
  resolved and the binding passes dt_binding_check.
- Do not enable panel or touch consumers until both regulator outputs are
  validated.
- Do not call this driver complete, compiled or hardware-tested.
- The current source is an implementation draft on the development branch,
  not yet a canonical git-format-patch email series.
