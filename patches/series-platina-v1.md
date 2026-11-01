# Platina v1 complete patch series

Target: upstream Linux v7.3.1

Apply in this exact order:

1. `0001-dt-bindings-arm-qcom-add-xiaomi-platina.patch`
2. `0002-arm64-dts-qcom-add-xiaomi-platina.patch`
3. `0003-arm64-dts-qcom-add-platina-dtb-target.patch`

The aggregate file `platina-v1-complete-series.patch` contains all three
patches in the same order and can be applied as one patch stream.

## Scope

- Add `xiaomi,platina` to the SDM660 board compatible schema.
- Add a conservative Platina board DTS using the SDM660, PM660 and PM660L
  SoC/PMIC descriptions and provisional BLSP1 UART2 console.
- Register `sdm660-xiaomi-platina.dtb` in the Qualcomm DTB Makefile.

## Validation status

- Individual patch hunk syntax and order checked against exact upstream
  v7.3.1 context excerpts in a local fixture.
- The complete three-patch series applied successfully to that context
  fixture with `git apply --check` and `git apply`.
- This is not a full kernel source checkout; DTS preprocessing, DTB build,
  schema validation and hardware boot validation remain pending.
