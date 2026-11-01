# Platina upstream driver compatibility audit

Audit target: upstream Linux v7.3.1.

This note compares the active vendor-kernel hardware description with the
available upstream drivers. A matching controller family name is not enough
to establish electrical or protocol compatibility.

## Novatek touchscreen

### Upstream support found

- Driver: `drivers/input/touchscreen/novatek-nvt-ts.c`
- Binding: `Documentation/devicetree/bindings/input/touchscreen/novatek,nvt-ts.yaml`
- Supported compatibles: `novatek,nt11205-ts` and
  `novatek,nt36672a-ts`.
- The NT36672A variant expects controller parameter-block chip ID `0x08`.
- The driver reads a parameter block beginning at register `0x78`, then
  reads six-byte contact records beginning at register `0x00`.
- The upstream implementation requests two regulators named `vcc` and
  `iovcc`, plus one reset GPIO and one interrupt.

### Platina runtime/vendor observations

- Active I2C device: bus 4, address `0x62`.
- Runtime name/modalias: `NVT-ts` / `i2c:NVT-ts`.
- Vendor OF compatible: `novatek,NVT-ts`.
- Vendor node uses a separate TDDI reset GPIO (TLMM 53), reset GPIO
  (TLMM 66), and IRQ GPIO (TLMM 67).
- Vendor power consumers include PM660L L11 for I/O and PM660L LCDB LDO/NCP
  outputs for the other panel/touch rails.

### Compatibility decision

**Do not bind the upstream driver directly yet.** The runtime compatible
string differs, the upstream driver has only one reset GPIO and two supply
inputs, and the vendor description has two reset-related GPIOs and three
supply consumers. The actual controller chip ID has not yet been confirmed
against upstream's expected `0x08`.

Next work must establish the controller ID and the exact purpose/timing of
both reset lines. If the controller reports chip ID `0x08`, adapt the
upstream driver deliberately for Platina's power/reset topology or prove that
one of the vendor reset lines is not required. Do not add an unverified
compatible string or omit a supply merely to make probe succeed.

## Tianma NT36672A display

### Upstream support found

- Driver: `drivers/gpu/drm/panel/panel-novatek-nt36672a.c`
- Binding: `Documentation/devicetree/bindings/display/panel/novatek,nt36672a.yaml`
- The current binding describes a Tianma FHD+ video panel at 1080 x 2246.
- Its supply interface is `vddio-supply`, `vddpos-supply` and
  `vddneg-supply`, plus a single reset GPIO and a DSI endpoint.
- The upstream driver uses its own initialization sequence and reset/power
  timing.

### Platina runtime/vendor observations

- Active panel: Tianma FHD NT36672A video panel.
- Resolution: 1080 x 2280 at 60 Hz.
- Vendor power rails: PM660 L11 for I/O, PM660L LCDB LDO for positive boost,
  and PM660L LCDB NCP for negative boost.
- Vendor initialization payload is 1621 bytes; the off payload is 54 bytes.
- Vendor reset sequence and LCDB enable/load timing are device-specific.

### Compatibility decision

**Do not use the generic upstream Tianma descriptor unchanged.** The
resolution differs (2280 versus 2246), and the initialization commands,
reset timing and power implementation have not been shown to match.
PM660L LCDB LDO/NCP support is also absent from the current upstream PM660L
DTS/driver baseline used in this port.

The correct path is a Platina-specific panel descriptor/driver adaptation,
with exact command bytes, packet types, lane/clock configuration, reset
timings and LCDB supplies preserved. A PM660L-specific LCDB regulator driver
and binding must be reviewed before enabling the panel's positive/negative
rails in DT.

## Current conclusion

Upstream contains useful Novatek driver foundations, but neither the active
touchscreen nor the panel is ready for a direct device-tree enablement.
Keep both nodes deferred until the driver and power topology are adapted and
reviewed. This is intentional protection against a display/touch regression,
not a claim that the hardware is unsupported in principle.


## Novatek upstream driver source-level review (2026-11-01)

The exact Linux v7.3.1 source was fetched and reviewed:

- Driver blob: `0f771f68195285f6c62304ea55e78e7cff15b598`.
- Binding blob: `bd6a60486d1f192d9655b50500afbf19a1dc4728`.
- The NT36672A match-data entry checks parameter-block byte offset
  `0x0e` for chip ID `0x08`; the block is read from register `0x78`.
  This is a concrete runtime discriminator, not something that can be
  inferred from the panel name or I2C address.
- The upstream probe enables `vcc` and `iovcc`, releases its single
  reset GPIO, waits 100 ms, reads controller parameters, then asserts reset
  and disables both regulators. Its runtime start path enables both rails,
  enables the IRQ and releases reset; stop disables IRQ, asserts reset and
  disables both rails.
- The upstream driver derives IRQ polarity/trigger from the controller
  parameter block. The binding allows one interrupt, one reset GPIO, and
  the two named supplies. It does not describe a second TDDI reset GPIO or
  the Platina LCDB LDO/NCP supply topology.
- The upstream PM path stops the input device and powers it down; it does
  not implement the vendor driver's wake-gesture mode. Therefore direct
  reuse would not preserve the currently observed Android gesture-wake
  behavior even if basic touch reporting worked.
- The upstream driver reports contacts using the standard input multitouch
  slot API and touchscreen properties. This is a useful implementation
  foundation, but does not prove that the vendor controller firmware uses
  the same parameter and contact-record protocol.

## Maintainer and submission route

The exact v7.3.1 `MAINTAINERS` entry for `NOVATEK NVT-TS I2C
TOUCHSCREEN DRIVER` lists:

- Maintainer: Hans de Goede <hansg@kernel.org>.
- List: `linux-input@vger.kernel.org`.
- Files: the Novatek binding and `drivers/input/touchscreen/novatek-nvt-ts.c`.

The broader input subsystem is maintained by Dmitry Torokhov and uses
`linux-input@vger.kernel.org`. The target tree's
`Documentation/process/submitting-patches.rst` requires one logical
change per patch, self-contained descriptions for each revision, style
checking with `scripts/checkpatch.pl`, and copying the appropriate
maintainer/list. Device-tree binding changes must additionally follow
`Documentation/devicetree/bindings/submitting-patches.rst`.

## Required next evidence before implementation

1. Read the actual controller parameter block on the running Platina device
   and record byte `0x0e`; do not assume it is `0x08`.
2. Establish whether TLMM 53 and TLMM 66 are both electrically required,
   including polarity, assertion order, delays and suspend states.
3. Trace which of the three vendor regulator consumers power the touch
   controller itself versus the display panel; do not collapse them into
   `vcc`/`iovcc` without rail measurements or vendor schematic/source proof.
4. Decide whether gesture wake is a required compatibility feature. If yes,
   design an explicit low-power/IRQ-wake state machine rather than silently
   inheriting the upstream power-off suspend path.
5. Only after the above evidence, prepare a narrowly scoped driver/binding
   adaptation and its tests. Keep the current board node disabled meanwhile.

No device I2C transaction, source build, binding validation or hardware test
was performed during this source review.
