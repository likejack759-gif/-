# PM660L LCDB driver design — implementation plan

Status: design stage; no driver code is implemented by this document.
Target: Linux v7.3.1 regulator framework and PM660L hardware evidence from
the vendor source commit recorded in pm660l-lcdb-porting-audit.md.

## 1. Architecture

Use one platform driver instance for the LCDB function block and register
two regulator devices (LDO and NCP). Keep PM660L register operations in
PM660L-specific helpers. Do not bind the PM8150L compatible or reuse its
register constants and electrical limits.

The state model must distinguish:

- requested_mask: regulator-core enable requests currently owned by consumers;
- module_enabled: physical shared LCDB module-enable state;
- output_good_mask: independently sampled LDO/NCP VREG_OK state;
- fault_latched: short-circuit lockout state;
- per-output requested voltage and validity;
- PMIC revision and whether the PM660L NCP workaround is required.

The request mask is not a substitute for hardware status. The shared module
enable is not proof that both outputs are valid. A bus read failure is an
error, not a disabled state.

## 2. Locking and execution context

All operations that change the shared boost, either output voltage, module
enable, fault state or request mask must be serialized by one sleepable
mutex. The short-circuit interrupt must use a threaded handler because the
recovery path performs SPMI/regmap operations and delays.

Do not hold the LCDB mutex while disabling/synchronizing an IRQ if the IRQ
thread can acquire that mutex. Probe/remove and fault teardown must follow
the IRQ synchronization ordering documented by the target kernel APIs.

## 3. Probe and bootloader handoff

Probe must:
1. acquire the parent SPMI regmap and PMIC revision through the upstream API;
2. validate PM660L subtype and revision;
3. parse the SPMI base and interrupt resources;
4. read module-enable, LDO VREG_OK and NCP VREG_OK independently;
5. read both output voltage selectors and boost state;
6. initialize requested_mask to no consumer requests (do not infer requests
   from bootloader hardware state);
7. preserve a bootloader-enabled module without an unnecessary power cycle;
8. register both regulator descriptors and the threaded fault IRQ;
9. unwind cleanly on every failure.

The interaction between bootloader-on outputs, regulator constraints
(boot_on/always_on), and later consumer enable/disable calls must be tested
against the v7.3.1 regulator core before finalizing exact handoff behavior.

## 4. Shared module enable/disable

The module may be disabled only when no output request remains and policy
allows shutdown. Enabling either output must ensure the shared module is
ready and that the requested output reaches its physical VREG_OK state.

A disable callback must not clear another output's request. A failed module
shutdown must not be reported as success or silently clear state. If the
hardware has partially transitioned, retain a conservative fault/unknown
state and require an explicit safe recovery path.

## 5. Voltage and boost transaction

The boost target is PM660L-specific. Use the vendor-derived 50 mV boost
step, 6.25 V ceiling and 200 mV default headroom only after validating the
full register encoding and all allowed rail combinations.

For every voltage request:
1. validate selector and range before any register write;
2. stage a copy of both output targets;
3. calculate the required boost from the staged targets;
4. read current register values needed for safe ordering;
5. raise boost before raising either output;
6. program only the relevant output selector(s);
7. lower boost only after output voltage reductions are complete;
8. commit software target state only after all required writes succeed.

A mutex serializes this sequence but cannot make several SPMI writes atomic.
The implementation must define which old register values can be restored
after a partial failure. If rollback itself fails, report the original and
rollback errors and mark the hardware state uncertain; do not pretend the
cached state still describes hardware.

The LDO and NCP regulator descriptors must use regulator-core voltage
mapping/listing callbacks that accurately represent the PM660L selector
table. Do not assume that the generic PM8150L linear ranges are applicable.

## 6. Current limit and soft start

The vendor driver exposes current-limit and soft-start controls. Before
exposing them upstream, verify each hardware selector table, reserved
encodings, valid ranges, default values, and whether the control is per-output
or shared. If a property is not needed by Platina and cannot be documented
cleanly, omit it from the initial driver rather than exposing an unsafe ABI.

## 7. Short-circuit handling

The threaded IRQ must preserve the PM660L status read/pre-write sequence and
revision-dependent NCP workaround. Fault handling must:
- confirm the fault after the hardware blanking interval;
- attempt shutdown and check its return value;
- never restart after an unsuccessful shutdown;
- apply the PM660L revision workaround exactly where required;
- bound retries using verified time/count semantics;
- latch a fault after persistent failures;
- notify regulator consumers only after the fault state is coherent;
- preserve all SPMI errors and prevent races with consumer callbacks.

## 8. Required test matrix

- Probe with module off, both outputs off.
- Probe with module on and only LDO VREG_OK.
- Probe with module on and only NCP VREG_OK.
- Probe with both outputs VREG_OK.
- Probe with inconsistent module-enable/status combinations.
- Enable LDO, enable NCP, disable LDO while NCP remains requested.
- Enable NCP, enable LDO, disable NCP while LDO remains requested.
- Disable the final request and verify shutdown ordering.
- Inject read/write failures at each step of boost and output programming.
- Inject failure during rollback and verify state becomes unknown/faulted.
- Verify voltage selector boundaries and selector/list/map round trips.
- Exercise PM660L revisions on both sides of the NCP workaround threshold.
- Trigger short-circuit once, repeatedly, during enable/disable and during
  voltage changes.
- Verify failed shutdown prevents restart and is surfaced to consumers.
- Test regulator constraints and boot_on/always_on handoff.
- Test module unload/remove and probe failure after each resource acquisition.

Where possible, add KUnit tests around pure selector/state-machine helpers
and use a fake regmap for deterministic register fault injection. Hardware
validation remains mandatory for actual analog sequencing and SC behavior.

## 9. Patch decomposition

The implementation series should be split into reviewable commits:
1. PM660L LCDB binding/schema (regulator DT binding convention and review).
2. PM660L regulator driver plus Kconfig/Makefile integration.
3. PM660L DTSI LCDB node, after binding and driver node shape agree.
4. Platina consumer supply mapping, only after display/touch sequencing is
   independently verified.

Each intermediate commit must be buildable and coherent. The binding and
driver must be reviewed by regulator and Devicetree maintainers as required;
the board DTS belongs in the Qualcomm SoC/DT tree or at the end of a combined
series.

## 10. Outstanding evidence before coding

- Exact upstream v7.3.1 source checkout and commit hash.
- Full PM660L vendor register definitions and all read/write call sites.
- PM660L VREG_OK register/bit mapping and read-clear/write-clear semantics.
- Exact secure-write coverage and whether key write must be repeated per
  protected register or transaction.
- PM660L BST voltage selector mapping and legal output/boost combinations.
- SC status sequence, interrupt type, revision workaround and retry behavior.
- Regulator-core boot_on/always_on registration semantics in v7.3.1.
- Current MAINTAINERS routing and current regulator/DT submission guidance.

Until these items are confirmed, this document is a design contract, not
a claim that the PM660L driver is implemented or safe to enable.


## Evidence resolved and remaining blocker

The target v7.3.1 PMIC API is now verified: qcom_pmic_get() returns
struct qcom_spmi_pmic; PM660L subtype is 0x1a; major is loaded from
PMIC_REV4. Vendor PM660L v2.0 uses REV4 value 0x02, and the NCP SCP
workaround is required when major < 0x02.

The output/boost selector mappings and the STS3/STS4 pre-write/read sequence
are recorded in docs/pm660l-lcdb-register-map.md and have host fixture tests.

One critical device-tree detail remains unresolved: the vendor LCDB node is
nested under USID 2 but its interrupt specifier names slave ID 3. The SPMI
interrupt binding defines the first cell as the slave ID. This must be
verified before authoring the upstream PM660L node or final binding example.
