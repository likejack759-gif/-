# Platina kernel patch submission and acceptance policy

Baseline: upstream Linux v7.3.1.
Scope: all kernel, driver, regulator, DRM/panel, input, power, networking,
audio, media, firmware-interface and Devicetree work in this port.

This policy is a project acceptance baseline. When a subsystem has more
specific current maintainer guidance, that guidance takes precedence. Before
working on a newly encountered subsystem or patch type, inspect its current
Documentation/process guidance, subsystem documentation, MAINTAINERS entries,
and recent accepted upstream examples. Record any additional requirements
here before implementing the patch.

## 1. Canonical patch artifact

- The canonical deliverable is a Git email patch generated from real commits
  with `git format-patch`, not a hand-written unified diff.
- Multi-patch series include `[PATCH 0/N]` cover letter and numbered patches.
- Record the exact base commit/tag and target tree. Do not claim a patch is
  based on v7.3.1 unless its base is the exact v7.3.1 source commit.
- Each commit must have a self-contained subject and changelog: problem,
  user-visible impact, design, hardware evidence, dependencies and tests.
- Separate independent logical changes. Keep every intermediate commit
  buildable and bisectable where practical.
- Do not fabricate From, Date, Message-Id, Signed-off-by, Acked-by,
  Reviewed-by, Tested-by or maintainer identities. Add DCO sign-off only
  after the actual author confirms the Developer's Certificate of Origin.
- Include a cover letter for multi-patch series. Explain series purpose,
  dependency/order, target tree, test coverage and known limitations.
- Include a base reference and use `git range-diff` when revising a series.

## 2. Devicetree-specific requirements

- Put binding/schema changes in a separate patch, normally before code that
  implements the binding. Use the subsystem's required subject prefix.
- New bindings use YAML Devicetree schema and the appropriate dual license
  (normally GPL-2.0-only OR BSD-2-Clause).
- Run `make dt_binding_check`; validate DTS instances with
  `make dtbs_check`; run `scripts/dtc/dt-check-style` where applicable.
- A compatible used in DTS must be documented by a binding first.
- DTS is driver-independent: submit DTS separately, or put DTS changes at
  the end of a combined series. DTS should be applied through the SoC/platform
  tree, not a driver subsystem tree.
- Do not add unverified supplies, GPIOs, clocks, resets, interrupts, pinctrl
  states, firmware names or status="okay" nodes.
- Preserve stable DT ABI and existing compatible semantics. A new compatible
  must describe a real hardware distinction and match a documented driver
  fallback/implementation strategy.

## 3. Driver/subsystem requirements

Before implementation, inspect the subsystem's own submission guide,
MAINTAINERS file, Kconfig conventions, binding conventions, locking and PM
expectations, plus recent accepted patches.

At minimum review:
- probe/remove and all partial-failure unwind paths;
- runtime/system suspend and resume;
- concurrency, locking, IRQ, workqueue and lifetime rules;
- bus/regmap error propagation and timeout handling;
- regulator/clock/reset sequencing and shared-resource ownership;
- firmware loading, ABI and userspace-visible interfaces;
- Kconfig dependencies, module/built-in combinations and Makefile ordering;
- kernel-doc or subsystem documentation for new interfaces.

Run relevant `scripts/checkpatch.pl`, sparse/clang-analyzer or other
subsystem static checks where available. Do not treat checkpatch as proof of
correctness.

## 4. Required evidence and validation report

Every series must identify:
- exact upstream baseline and source commit;
- device/hardware evidence and source provenance;
- files changed and patch dependency graph;
- commands actually executed, environment and exit status;
- build/configuration targets and warnings;
- schema/test results, runtime evidence and known gaps;
- rollback/revert procedure and safety constraints.

Validation labels:
- L0: patch content and metadata reviewed;
- L1: formatting/static checks actually run;
- L2: full target-tree `git apply --check` and application;
- L3: relevant kernel/DT build and schema checks;
- L4: device boot and hardware-function tests.

A result at one level does not imply a higher level. Fixture/context tests
must be labelled as fixture tests, never full-tree application tests.
Never claim build, test, boot or flash success without captured evidence.

## 5. Project delivery layout

For each feature, keep:
- `0000-cover-letter.patch` for a multi-patch series;
- numbered `0001-*.patch`, `0002-*.patch`, etc.;
- `README.md` with base, apply order, dependencies and revert instructions;
- `hardware-evidence.md` with confirmed hardware facts and provenance;
- `validation-report.md` with actual commands/results;
- `SHA256SUMS` for the final artifacts.

The patch files must be generated from the actual commit series. Supporting
reports do not replace the patch artifacts.

## 6. New issue / new subsystem rule

If a new problem is outside this policy:
1. identify its subsystem and maintainers using `MAINTAINERS`;
2. read current upstream submission and subsystem-specific documentation;
3. inspect relevant bindings/APIs and recent accepted patches;
4. derive extra tests, metadata, ordering and safety rules;
5. update this policy and the feature acceptance checklist;
6. only then implement and package the patch.

## References

- https://docs.kernel.org/process/submitting-patches.html
- https://docs.kernel.org/devicetree/bindings/submitting-patches.html
- https://docs.kernel.org/devicetree/bindings/writing-schema.html
- https://docs.kernel.org/process/maintainer-handbooks.html
- https://docs.kernel.org/driver-api/
