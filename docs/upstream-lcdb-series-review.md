# Upstream Qualcomm LCDB series review — PM660L relevance

Audit date: 2026-11-01
Target baseline: stable Linux v7.3.1.

## Findings that change the implementation plan

A Qualcomm LCDB driver series was posted on 2026-10-15, followed by v2
on 2026-10-19:

- v1 driver: [PATCH 5/9] regulator: add Qualcomm LCDB driver.
- v2 driver: [PATCH v2 4/8] regulator: add Qualcomm LCDB driver.
- v2 binding: [PATCH v2 2/8] regulator: dt-bindings: add Qualcomm LCDB regulator.

The series explicitly supports PMI632, PM6150L and PM8150L, and explicitly
excludes PM660L because its LCDB boost programming differs. The binding
describes PM8150L as the base compatible with PMI632/PM6150L fallbacks; it
does not describe PM660L.

The exact target baseline gregkh/linux:v7.3.1 does not contain
drivers/regulator/qcom-lcdb-regulator.c (GitHub contents lookup returned
404). Therefore the posted series is a reference under review, not code
already available in the v7.3.1 baseline. Do not claim it is merged or part
of the target tree.

## Reusable design ideas

The v2 driver provides useful generic architecture:
- one provider object for the LCDB block;
- one mutex serializing shared boost and output operations;
- per-output enable request mask;
- regulator descriptors using linear voltage ranges and selector callbacks;
- compute boost from the higher output plus headroom;
- raise boost before output voltage changes and lower it afterwards;
- ramp from a lower voltage on first enable;
- wait for VREG_OK and apply off/on delay;
- short-circuit retry policy and regulator notifier events;
- update cached requested voltage only after successful register programming.

These are design references only. Do not copy PM8150L register constants,
voltage steps, headroom, boost limits, timing, interrupt handling or
compatible strings into PM660L without PM660L-specific evidence.

## Review findings in the posted v2 draft

The following issues must be explicitly addressed in our PM660L design rather
than assuming the posted draft is a complete correctness template:

1. Its hardware-init path reads the shared module-enable bit and, when set,
   marks both output bits enabled. This can misrepresent bootloader state
   if only one rail is actually valid. PM660L must read each output's own
   VREG_OK status, where the hardware exposes it, and distinguish physical
   state from consumer request state.
2. The v2 is_enabled callback reports the software enabled mask, not
   independently verified physical VREG_OK status. Decide and document
   the correct provider semantics for PM660L; propagate read errors if
   physical status is queried.
3. Short-circuit recovery calls module-disable but ignores its return value
   before proceeding with retry accounting/re-enable. PM660L implementation
   must preserve the original failure and enter a safe fault state if shutdown
   did not complete.
4. The shared output/boost programming is serialized, and v2 stages the new
   requested voltage before committing the cache. Retain the transactional
   cache behavior, but design PM660L-specific rollback for partial register
   writes; a mutex alone does not make a multi-register transaction atomic.
5. The v2 driver uses a single module enable and an enabled mask. That pattern
   is a useful starting point, but PM660L's extra current-limit, soft-start,
   secure-write and revision-specific SC behavior require a separate hardware
   operations layer or explicit PM660L-specific implementation.

These are code-review observations of a mailing-list patch, not statements
that the series has been accepted or merged.

## PM660L-specific differences already evidenced by vendor source

- PM660L secure-write key: write 0xa5 at block base + 0xd0 before protected
  register writes.
- Boost voltage step: 50 mV; hardware ceiling 6.25 V.
- Default boost headroom: 200 mV.
- NCP short-circuit workaround depends on PM660L revision (below v2.0).
- Vendor supports additional current-limit and soft-start controls that the
  generic v2 driver intentionally omits.
- Platina display and touch both consume the LCDB positive/negative rails.

Use the existing docs/pm660l-lcdb-porting-audit.md for the complete
vendor-source evidence and register audit.

## Updated implementation direction

Do not fork the PM8150L driver by changing only the compatible string.
Implement PM660L-specific register operations and state handling, while
reusing only proven regulator-core patterns. The implementation must include:
- distinct requested/enabled state per output;
- actual per-output physical status reconciliation at probe;
- shared module enable ownership;
- PM660L secure-write handling;
- PM660L boost voltage and headroom algorithm;
- NCP revision-specific SC workaround;
- correct current-limit and soft-start selector encoding;
- transactional voltage state and safe partial-failure handling;
- SC IRQ recovery that checks every shutdown/restart result;
- complete probe and devres error handling.

## References

- v1 driver patch (2026-10-15):
  https://lists.openwall.net/linux-kernel/2026/10/15/988
- v2 driver patch (2026-10-19):
  https://go.mail-archive.com/dri-devel%40lists.freedesktop.org/msg645175.html
- v2 binding patch (2026-10-19):
  https://go.mail-archive.com/dri-devel%40lists.freedesktop.org/msg645238.html
- Regulator API:
  https://docs.kernel.org/driver-api/regulator.html
- Regulator driver interface:
  https://docs.kernel.org/power/regulator/regulator.html
