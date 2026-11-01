#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare a clean Linux v7.3.1 tree with the PM660L LCDB draft sources.

This script only stages source-file integration. It does not commit, build,
flash, enable the regulator node in a board DTS, or generate format-patch.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

TARGET_COMMIT = "79a907609d53d5cd645b42c11550d295d43abf99"


def run_git(tree: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(tree), *args],
        check=True, text=True, capture_output=True,
    )
    return result.stdout.strip()


def unique_anchor(content: str, anchor: str, path: Path) -> int:
    count = content.count(anchor)
    if count != 1:
        raise RuntimeError(
            f"{path}: expected exactly one insertion anchor {anchor!r}, found {count}"
        )
    return content.index(anchor)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("kernel_tree", type=Path,
                        help="clean Linux v7.3.1 source checkout")
    args = parser.parse_args()

    project = Path(__file__).resolve().parents[1]
    kernel = args.kernel_tree.resolve()

    if not (kernel / ".git").exists():
        raise RuntimeError(f"{kernel} is not a Git working tree")

    head = run_git(kernel, "rev-parse", "HEAD")
    if head != TARGET_COMMIT:
        raise RuntimeError(
            f"wrong kernel base: expected {TARGET_COMMIT}, found {head}"
        )

    dirty = run_git(kernel, "status", "--porcelain")
    if dirty:
        raise RuntimeError(
            "kernel tree is not clean; commit/stash/back up existing changes first"
        )

    sources = {
        "drivers/regulator/qcom-pm660l-lcdb-regulator.c":
            project / "drivers/regulator/qcom-pm660l-lcdb-regulator.c",
        "drivers/regulator/qcom-pm660l-lcdb-internal.h":
            project / "drivers/regulator/qcom-pm660l-lcdb-internal.h",
        "Documentation/devicetree/bindings/regulator/qcom,pm660l-lcdb-regulator.yaml":
            project / "Documentation/devicetree/bindings/regulator/qcom,pm660l-lcdb-regulator.yaml",
    }
    for rel, src in sources.items():
        if not src.is_file():
            raise RuntimeError(f"missing project source: {src}")
        if (kernel / rel).exists():
            raise RuntimeError(f"refusing to overwrite existing file: {kernel / rel}")

    kconfig_path = kernel / "drivers/regulator/Kconfig"
    makefile_path = kernel / "drivers/regulator/Makefile"
    dtsi_path = kernel / "arch/arm64/boot/dts/qcom/pm660l.dtsi"

    kconfig = kconfig_path.read_text()
    makefile = makefile_path.read_text()
    dtsi = dtsi_path.read_text()

    kconfig_fragment = (project / "patches/pm660l/Kconfig.fragment").read_text().strip()
    makefile_fragment = (project / "patches/pm660l/Makefile.fragment").read_text().strip()
    dtsi_fragment = (project / "patches/pm660l/pm660l-usid3-node.fragment").read_text().strip()

    if "REGULATOR_QCOM_PM660L_LCDB" in kconfig:
        raise RuntimeError("PM660L LCDB Kconfig entry already exists")
    if "qcom-pm660l-lcdb-regulator.o" in makefile:
        raise RuntimeError("PM660L LCDB Makefile entry already exists")
    if "qcom,pm660l-lcdb-regulator" in dtsi:
        raise RuntimeError("PM660L LCDB DTSI node already exists")
    if "#include <dt-bindings/interrupt-controller/irq.h>" not in dtsi:
        raise RuntimeError("PM660L DTSI lacks the IRQ_TYPE_EDGE_RISING include")

    # Calculate all edits before writing anything.
    kconfig_anchor = kconfig.rfind("\nendif")
    if kconfig_anchor < 0:
        raise RuntimeError("drivers/regulator/Kconfig: final endif not found")

    make_anchor = "obj-$(CONFIG_REGULATOR_QCOM_LABIBB) += qcom-labibb-regulator.o"
    make_index = unique_anchor(makefile, make_anchor, makefile_path) + len(make_anchor)

    dtsi_anchor = "\t\tpm660l_spmi_regulators: regulators {"
    dtsi_index = unique_anchor(dtsi, dtsi_anchor, dtsi_path)

    new_kconfig = (
        kconfig[:kconfig_anchor] + "\n\n" + kconfig_fragment +
        "\n" + kconfig[kconfig_anchor:]
    )
    new_makefile = (
        makefile[:make_index] + "\n" + makefile_fragment +
        makefile[make_index:]
    )
    new_dtsi = (
        dtsi[:dtsi_index] + dtsi_fragment + "\n\n" +
        dtsi[dtsi_index:]
    )

    for rel, src in sources.items():
        dst = kernel / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    kconfig_path.write_text(new_kconfig)
    makefile_path.write_text(new_makefile)
    dtsi_path.write_text(new_dtsi)

    subprocess.run(
        ["git", "-C", str(kernel), "diff", "--check"],
        check=True,
    )

    print("PM660L LCDB draft sources integrated.")
    print(f"Kernel base: {head}")
    print("Changed files:")
    for rel in [*sources, "drivers/regulator/Kconfig",
                "drivers/regulator/Makefile",
                "arch/arm64/boot/dts/qcom/pm660l.dtsi"]:
        print(f"  {rel}")
    print("The PM660L LCDB node remains disabled; no Platina board enablement was added.")
    print("No commit, build, dt_binding_check, dtbs_check, or format-patch was run.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
