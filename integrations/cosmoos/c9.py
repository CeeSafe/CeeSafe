#!/usr/bin/env python3
"""
c9.py -- CeeSafe milestone C9: CosmoOS's GPT parser and writer (kernel/block/gpt.c) reimplemented in CeeSafe
(CosmoOS's kernel/block/gpt.cee, compiled by ceesafec from a .cee copy), held to CosmoOS's own tests, a differential test and C4's kernel-object checks
(docs/cosmoos/c9-report.md). The steps are C6's (c6.py), with no QEMU:
  1. ceesafec compiles the port (--profile freestanding --no-float) to C exporting gpt.h's functions and data;
  2. CosmoOS's own host test (tests/host/test_gpt.c) and fuzz target (tests/fuzz/fuzz_gpt.c) are built twice
     with its HOST_CFLAGS / FUZZ_CFLAGS (ASan, UBSan, -Werror): with gpt.c, and with the CeeSafe object in its place;
  3. the differential test (c9/diff_gpt.c) runs both on the same disks: identical results, reasons, tables,
     scratch buffers and reads;
  4. the CeeSafe object is compiled with CosmoOS's MODULE_CFLAGS and checked as C4 does.
The benchmark is benchmarks/gpt/run.py.

Usage:
    python3 integrations/cosmoos/c9.py [--arch aarch64|x86_64] [--cosmoos ~/dev/CosmoOS] [--fuzz-runs 20000] [--cases 20000]
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import c1  # noqa: E402
import c4  # noqa: E402
import c6  # noqa: E402

C9_DIR = Path(__file__).resolve().parent / "c9"

# C9b: CosmoOS builds the CeeSafe port itself: kernel/block/gpt.cee, beside the gpt.c that `make cee` generates from
# it. Its C original is then the file at the last commit that had it, and the runtime files the tree adds are not
# taken from the tree.
ORIGINAL_COMMIT = "dc1d5166"
IN_TREE_RUNTIME = ["kernel/ceesafe/ceesafe_rt_core.c", "tests/host/ceesafe_panic.c"]  # CosmoOS's runtime files

# As c6.COMPONENTS: the CeeSafe package (here relative to the CosmoOS tree, made absolute in main), the C original, the Make variables of the host test and fuzz sources, the C
# symbols renamed for the differential test, the differential test, and the host test's extra flags.
COMPONENTS = {
    "gpt": ("kernel/block/gpt.cee", "kernel/block/gpt.c", "HOST_GPT_SRCS", "FUZZ_GPT_SRCS",
            ["gpt_parse", "gpt_build", "gpt_build_first_usable", "gpt_build_last_usable", "gpt_crc32_update", "gpt_crc32",
             "gpt_guid_format", "gpt_guid_parse", "gpt_type_esp", "gpt_type_cosmo_root"], "diff_gpt.c",
            ["-DCMDLINE_HOST_TEST=1"]),  # tests/host/host.mk's rule for test_gpt
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cosmoos", default=str(Path.home() / "dev" / "CosmoOS"))
    parser.add_argument("--arch", choices=["x86_64", "aarch64"], default="aarch64", help="for the kernel-flag checks")
    parser.add_argument("--fuzz-runs", type=int, default=20000)
    parser.add_argument("--cases", type=int, default=20000)
    args = parser.parse_args()
    root = Path(args.cosmoos).expanduser().resolve()
    c4.COSMO = root
    status_before = c1.git_status(root)
    cosmo = c1.Cosmo(root, args.arch, c1.CEESAFE_ROOT / "out" / "cosmoos" / f"{args.arch}-release")
    c1.require_compiler()
    checks = {}
    original_file, in_tree = None, ()
    if "GENERATED: do not edit" in (root / "kernel/block/gpt.c").read_text():
        original_file = c1.CEESAFE_ROOT / "out" / "cosmoos" / "c9" / "gpt_original.c"
        original_file.parent.mkdir(parents=True, exist_ok=True)
        original_file.write_text(subprocess.run(["git", "-C", root, "show", f"{ORIGINAL_COMMIT}:kernel/block/gpt.c"],
                                                check=True, capture_output=True, text=True).stdout)
        in_tree = IN_TREE_RUNTIME
        checks[f"C original from CosmoOS {ORIGINAL_COMMIT} (the tree builds the CeeSafe port)"] = (True, [])
    components = {name: (str(root / package), *rest) for name, (package, *rest) in COMPONENTS.items()}
    for name, (package, *_) in components.items():
        if not Path(package).exists():
            sys.exit(f"[c9] {package} is missing: the CeeSafe sources live in CosmoOS since A5's first PR (#461)")
    for name in components:
        c6.check_component(name, cosmo, root, args, checks, components=components, milestone="c9", diff_dir=C9_DIR,
                           original_file=original_file, in_tree_runtime=in_tree)
    checks["CosmoOS working tree unchanged"] = (c1.git_status(root) == status_before, [])
    for name, (ok, details) in checks.items():
        print(f"[c9] {'PASS' if ok else 'FAIL'}  {name}")
        for detail in details[:10]:
            print(f"[c9]        {detail}")
    return 0 if all(ok for ok, _ in checks.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
