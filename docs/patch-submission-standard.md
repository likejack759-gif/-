# Kernel patch submission and acceptance standard

This document governs all future Xiaomi Mi 8 Lite (platina) kernel-port
patches. The target baseline is upstream Linux v7.3.1 unless a later
decision explicitly changes it.

## 1. Rule for newly encountered subsystems

Before implementing a new issue, identify the affected kernel subsystem and
consult its current in-tree documentation, MAINTAINERS entries, maintainer
tree/workflow, and relevant binding or driver documentation. If the existing
project checklist does not cover the issue, add the subsystem-specific
requirements here or in a linked subsystem checklist before treating the
patch as ready.

Do not assume that one generic patch checklist covers every subsystem.
Maintainer instructions for the target tree take precedence where they
specify a different workflow.

## 2. Canonical patch format

- Generate upstream-submission patches from Git commits with
  `git format-patch`; do not hand-label a plain unified diff as a formal
  mail patch.
- Use the canonical subject form `[PATCH n/N] subsystem: summary`.
  Keep the subject within the kernel's recommended 80-character budget.
- Explain the motivating problem, user-visible impact, implementation,
  hardware evidence, limitations, and tests in the commit message.
- Keep the permanent commit message self-contained and wrap the explanation
  at approximately 75 columns.
- Solve one logical problem per patch. Split independent changes; keep
  required multi-file changes together when they implement one coherent
  function.
- Provide a cover letter for a multi-patch series, with patch order,
  dependencies, baseline commit/tag, and validation summary.
- Include a base commit or exact baseline tag/commit so reviewers can
  reproduce the series.
- Add `Signed-off-by:` only when the actual submitter has confirmed the
  Developer Certificate of Origin. Never fabricate sign-offs, Acked-by,
  Reviewed-by, or Tested-by tags.
- Preserve SPDX identifiers, copyright notices, authorship, and license
  compatibility. Do not copy vendor code without checking its license and
  provenance.
- Run `scripts/checkpatch.pl`; justify remaining CHECK/WARNING reports
  instead of blindly changing code to silence the tool.
- Use `MAINTAINERS` and `scripts/get_maintainer.pl` to identify recipients
  before any real upstream submission.

## 3. Device Tree requirements

- Follow `Documentation/devicetree/bindings/submitting-patches.rst` and
  `writing-schema.rst` from the exact target tree.
- New or changed bindings use YAML/JSON Schema and the required license
  expression. Run `make dt_binding_check`.
- Every DTS compatible must be documented by the appropriate binding.
- Run `make dtbs_check` and the relevant DTS style checks.
- Keep binding changes separate from DTS changes where the DT workflow
  requires it. DTS is generally submitted independently; if combined with
  driver changes, place DTS changes at the end unless the relevant
  maintainers specify another ordering.
- Do not enable a node until its driver, supplies, clocks, resets, GPIOs,
  interrupts, pinctrl, firmware and power sequencing are evidenced.
- Do not add speculative properties or copy another board's wiring merely
  because it shares the SoC or controller family.

## 4. Driver and subsystem requirements

For every driver change, inspect the subsystem's own documentation and
maintainer workflow. At minimum, review:

- probe, error unwind, remove and resource lifetime;
- concurrency, locking, IRQ and workqueue interactions;
- runtime/system PM, suspend/resume and wakeup;
- register access errors, timeouts, rollback and hardware state recovery;
- regulator/clock/reset dependencies and shared-resource semantics;
- Kconfig help, module build, configuration combinations and dependencies;
- ABI and documentation impact, including sysfs, debugfs, procfs, ioctl,
  module parameters and kernel parameters;
- firmware licensing, naming, request path and failure behavior;
- compatibility with the target kernel APIs, not just vendor-kernel APIs.

Do not claim a generic checklist is sufficient for specialized subsystems
such as DRM, input, regulator, power-supply, media, ASoC, remoteproc, WLAN,
Bluetooth, USB or networking. Add their official subsystem-specific
requirements when those areas are reached.

## 5. Required validation levels

Record each check separately and include exact commands, source commit,
configuration, relevant output and unresolved warnings.

- L0 — source provenance, patch completeness, manual design/code review.
- L1 — patch formatting and static checks, including checkpatch and
  subsystem-specific static analysis where available.
- L2 — apply the complete series to the exact target source tree; verify
  with `git am` or `git apply --check` as appropriate.
- L3 — relevant Kconfig/build targets, DT schema checks, DTB build,
  sparse and other subsystem-required tests.
- L4 — device boot and real hardware function, error paths, suspend/resume,
  thermal/power behavior and regression checks.

A lower level must never be described as a higher one. A fixture containing
selected upstream context is only a fixture test, not a complete source-tree
application test. A successful build is not proof of hardware operation.

## 6. Delivery contents

Each feature series must include:

- standard numbered patch files generated from commits;
- a cover letter for multi-patch series;
- a README with baseline, dependencies, apply/revert instructions;
- hardware/source evidence and provenance;
- a validation report with exact commands and results;
- SHA-256 checksums for delivered artifacts.

A plain aggregate diff may be retained for convenient review, but it must be
labelled as a review diff and not as a canonical upstream mail patch series.

## 7. Safety and rollback

- Keep work on the dedicated development branch until review and validation
  are complete; do not silently change the production/main branch.
- Preserve a known-good boot image and a recoverable test route.
- Never write modem, persist, modemst or other sensitive calibration/
  identity partitions as part of kernel testing.
- Do not bypass thermal protections or apply unverified overclocking.
- Clearly separate source changes, build artifacts, boot tests and flashing.
  No flash operation without an explicit, separately confirmed plan.

## 8. Current Platina-specific sequencing

The initial board description is a conservative skeleton, not a complete
hardware port. Before adding a peripheral, complete its hardware evidence,
upstream compatibility audit, driver/binding plan and subsystem-specific
requirements. For PM660L LCDB, preserve independent LDO/NCP state while
handling the shared module enable, propagate SPMI failures, implement safe
short-circuit recovery, and fully unwind probe failures before enabling
panel/touch consumers.
