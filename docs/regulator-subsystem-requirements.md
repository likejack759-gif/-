# Regulator subsystem requirements for Platina

Target: upstream Linux v7.3.1 regulator framework.

This checklist was added after the PM660L LCDB audit identified a shared
hardware-enable line behind two independently registered regulator outputs.
It supplements `docs/patch-submission-standard.md`.

## Official references consulted

- `Documentation/power/regulator/` in the exact target kernel tree.
- Linux regulator driver interface documentation:
  https://docs.kernel.org/driver-api/regulator.html
- Linux regulator framework concepts and consumer reference counting:
  https://docs.kernel.org/power/regulator/regulator.html
- Device Tree binding submission and schema rules:
  `Documentation/devicetree/bindings/submitting-patches.rst`.

The regulator framework reference-counts enable/disable requests for each
regulator. A consumer must balance its calls. Multiple consumers may share a
regulator, and a disable request does not necessarily mean the physical rail
is turned off. A provider must therefore distinguish framework-level
per-regulator requests from hardware resources shared between outputs.

## Mandatory review points

1. Model each independently registered output's requested/enabled state.
2. Model shared hardware resources separately from per-output state.
3. Enable the shared block on the first active output; disable it only after
   the last output has been successfully disabled.
4. Do not update software state until the corresponding hardware transition
   succeeds. On failure, preserve or restore the prior state.
5. Reconcile bootloader-initialized hardware state with each output's own
   status; a shared module-ready bit does not prove every output is valid.
6. Implement `.is_enabled` using each output's real hardware status, and
   propagate bus read errors rather than converting them to false.
7. Serialize shared-register operations and define lock ordering with IRQ,
   threaded IRQ, workqueue, PM and sysfs paths.
8. Ensure short-circuit/fault recovery does not erase consumer-request state.
   Define behavior when a fault permanently locks out the block.
9. Handle suspend/resume and secure/firmware-owned modes without returning
   false success or desynchronizing regulator-core state.
10. Validate voltage/current selector ranges before encoding; preserve the
    exact datasheet/vendor mapping and reject unsupported values.
11. Check all register writes and implement safe rollback for partial
    enable/ramp/configuration failures.
12. Ensure regulator registration, IRQ registration, class/sysfs creation
    and probe failure unwind are complete and ordered.
13. Validate constraints and consumer supply names against the actual board
    wiring. Never enable a consumer before its supply provider is ready.

## PM660L LCDB-specific state model to implement

- LDO and NCP are independently registered voltage regulators.
- Their hardware module enable is shared.
- Each output has its own VREG_OK status bit.
- The driver must maintain independent LDO/NCP request state and a shared
  active-block state, protected by a single clearly defined synchronization
  strategy.
- Short-circuit recovery may cycle the hardware block but must preserve the
  active request set and re-establish only the outputs that consumers still
  require.
- If the hardware cannot safely restore the previous state after a fault,
  return an error and leave the state explicitly faulted; never claim that
  the regulator was enabled.

## Required tests

- LDO enable -> NCP enable -> LDO disable: NCP remains powered.
- NCP enable -> LDO enable -> NCP disable: LDO remains powered.
- Last active output disable: shared block powers down.
- Failure at each register access during enable/disable: error propagation
  and state consistency.
- One VREG_OK asserted while the other is clear: independent status results.
- SC IRQ during enable/disable and during recovery: no deadlock or lost
  request state.
- Probe with module off, module on with LDO only, NCP only, and both outputs.
- Probe failure after each resource registration: no leaked class, IRQ or
  regulator registration.
- Suspend/resume and secure-mode transitions: no false-success callbacks.
- Repeated consumer enable/disable cycles: regulator-core counts and hardware
  state remain consistent.

No implementation is considered complete until relevant tests and build
checks have been recorded against the exact v7.3.1 source tree.
