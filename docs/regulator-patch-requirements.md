# Regulator subsystem patch requirements — PM660L LCDB

Target: upstream Linux v7.3.1 regulator framework.
Device: Qualcomm PM660L LCDB positive LDO and negative charge-pump outputs.
Status: acceptance checklist only; this is not a driver implementation.

## Maintainer and patch routing

- Check the target tree's `MAINTAINERS` entries for regulator, Qualcomm PMIC,
  SPMI and Devicetree ownership before preparing the final Cc list.
- Follow regulator subsystem submission conventions and the current regulator
  maintainer tree. Do not assume a generic DT-only route is sufficient.
- For regulator DT binding patches, check the regulator-specific subject
  prefix convention. Current DT submission guidance identifies regulators as
  one of the subsystems that use the reversed form:
  `<binding dir>: dt-bindings: ...`.
- Keep the regulator driver and its binding logically associated. If the
  binding is submitted through the regulator tree, obtain the appropriate DT
  review/Ack as required by the current maintainer workflow.
- The Platina board DTS change is separate and routed through the Qualcomm
  SoC/DT tree; do not apply board DTS through a regulator driver tree.

## Regulator-core semantic requirements

- Each regulator output must have correct independent regulator-core
  reference-count semantics, even though the PM660L LCDB hardware module
  enable is shared.
- Track per-output active requests and only disable the shared module after
  the last output request is removed.
- Do not confuse regulator-core use counts with physical VREG_OK status.
- `is_enabled` must report the state of the individual output and propagate
  negative SPMI/regmap errors. Never translate a bus error into disabled.
- Handle firmware/bootloader pre-enabled outputs without claiming that both
  rails are enabled merely because the shared module is on.
- Avoid regulator_force_disable semantics for ordinary consumer disable
  paths; it bypasses shared consumer ownership and must be reserved for a
  documented emergency condition.
- Voltage/current selectors and constraints must match PM660L hardware
  encodings, ranges, steps and reserved values exactly.
- Validate all arguments before register writes. Propagate errors and roll
  back partially completed multi-register transitions where safe.
- Shared-state locking must cover request-state and hardware transitions.
  IRQ recovery, secure mode and regulator callbacks must not deadlock or
  silently desynchronize regulator-core state.

## PM660L hardware-specific requirements

- Use the PM660L-specific protected-write sequence and register map.
- Use the PMIC revision API supported by the target kernel; do not carry
  vendor-only revision structures into upstream code.
- Preserve the PM660L-specific voltage step, ceiling, headroom and revision
  workarounds established by source evidence.
- Keep the short-circuit IRQ status read/pre-write sequence and recovery
  behavior faithful to verified PM660L hardware behavior.
- Do not substitute a PM8150L or other LCDB implementation merely because
  register names look similar.
- Preserve LDO and NCP output independence while respecting the shared
  module-enable bit.

## Binding and DTS requirements

- Add a dedicated YAML schema for the PM660L LCDB parent and its LDO/NCP
  child regulator nodes, consistent with existing Qualcomm PMIC bindings.
- Define the correct SPMI child address/size, interrupt, interrupt-name,
  PMIC revision relationship and regulator constraints only when supported
  by the target PMIC binding conventions.
- Validate with `make dt_binding_check` and `make dtbs_check`.
- Add the PM660L block to `pm660l.dtsi` only after the driver and schema
  agree on node shape and register layout.
- Add Platina consumers only after exact supply mapping and sequencing are
  verified. Do not enable the display/touch panel nodes prematurely.

## Required code review and tests

- Probe success and every probe failure point, including registration and
  hardware initialization failures.
- Remove/unbind and devres cleanup.
- Independent LDO enable/disable; independent NCP enable/disable; both rails
  on; disable either rail while the other remains on; disable last rail.
- Pre-enabled LDO only, NCP only, both enabled and inconsistent hardware
  states.
- SPMI read/write failures at every state transition.
- Voltage/current boundary values, invalid values and selector round trips.
- Short-circuit IRQ, repeated fault, recovery, secure-mode transition and
  IRQ concurrency.
- Suspend/resume and reboot/bootloader handoff state if the hardware block
  remains active across those transitions.
- Run regulator-focused KUnit tests where suitable, relevant build configs,
  `scripts/checkpatch.pl`, sparse and lockdep-enabled runtime tests where
  available. Add tests for the state machine where the existing test
  infrastructure permits it.

## Required patch-series contents

As applicable:
1. regulator binding schema;
2. PM660L regulator driver;
3. Kconfig and Makefile integration;
4. PM660L DTSI node;
5. consumer DTS updates in a separate SoC-tree patch or at the end of a
   combined series;
6. cover letter, hardware evidence, validation report and checksums.

The sequence must leave every intermediate commit buildable and coherent.
No patch may be described as complete until full target-tree apply, relevant
build/schema checks and the available regulator tests are reported.

## References

- https://docs.kernel.org/driver-api/regulator.html
- https://docs.kernel.org/power/regulator/
- https://docs.kernel.org/devicetree/bindings/submitting-patches.html
- https://docs.kernel.org/process/maintainer-soc.html


## Upstream LCDB reference review

An October 2026 upstream LCDB driver and binding series covers PMI632,
PM6150L and PM8150L, but explicitly excludes PM660L because its boost
programming differs. The proposed driver is not included in the v7.3.1
baseline and must be treated as a design reference, not copied as a driver.

The PM660L implementation must additionally:
- distinguish each output's physical status from the shared module-enable bit;
- keep consumer request state separate from hardware output-good state;
- check all shutdown and restart operations during short-circuit recovery;
- commit cached voltage requests only after successful register programming;
- define safe handling for partial multi-register updates;
- avoid importing PM8150L voltage, boost, timing or fault parameters.

Detailed review: docs/upstream-lcdb-series-review.md.
