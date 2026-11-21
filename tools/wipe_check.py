#!/usr/bin/env python3
"""The wipe is kept (ADR 0197 §4.4): `Secret.wipe` survives the C compiler's optimiser, where plain stores do not.

Two programs, built as ADR 0197 §1.3 measured them, each with the secret on the stack:
- `key`: a `Key { bytes: [u8; 32] }` whose `drop` calls `Secret.wipe(mut this.bytes)`, filled by `Random.fill`,
  summed and dropped;
- `aes`: `Aes256Gcm.create(key)` in a function, one `seal`, the tag returned; lib/std's drop wipes the schedule.

Each is built at -O0, -O2, -O3 and -O2 -flto, with every C compiler found (clang; a real gcc, not Apple's alias of
clang; cl is not looked for). In the disassembly (objdump), some function must call, after its last use of the
secret (marked by a call of `wipe_check_sink`, a C function of the check's own, out of line, that takes a number
computed from the secret), a function that reaches `ceesafe_wipe`: the wipe is performed after the last use, at
every level. A call is followed three deep, so an out-of-line drop counts as the wipe. `ceesafe_wipe` is out of line
(`noinline`) for this, also under LTO.

The control, `key` with a loop of plain stores in its `drop` instead of `Secret.wipe`, is built the same way: it
must never reach `ceesafe_wipe` (else the analysis would pass vacuously), and the check reports whether any zeroing
(a store of a zero register, `bzero` or `memset`) is left after the last use. At -O2 and above the stores are
expected gone, as §1.3 measured; when a compiler keeps them, that is reported, not counted as a failure.

usage: python3 tools/wipe_check.py --compiler out/ceesafec [--objdump objdump]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

SINK = """#include <stdint.h>
/* The programs' last step after the secret's last use (tools/wipe_check.py): a call no optimiser removes or inlines,
   so the disassembly has a fixed point to look after, at every level and under LTO. */
__attribute__((noinline)) void wipe_check_sink(uint64_t value) {
    __asm__ __volatile__("" : : "r"(value) : "memory");
}
"""

EXTERN = """extern "C" {
    @effects()
    fn wipe_check_sink(value: u64);
}

"""

KEY = EXTERN + """struct Key {
    bytes: [u8; 32];

    drop {
        WIPE
    }
}

/// Safety: the sink only reads its number.
fn useKey() -> u32 {
    var k = Key { bytes: [0u8; 32] };
    let _ = Random.fill(mut k.bytes[..]);
    var sum: u32 = 0;
    for (b in k.bytes) { sum += b as u32; }
    unsafe { wipe_check_sink(sum as u64); } // after the key's last use, before its drop
    return sum;
}

fn main() {
    Console.writeLine(useKey() < 100000);
}
"""

AES = EXTERN + """/// Safety: the sink only reads its number.
fn sealOnce(key: Slice<u8>, data: SliceMut<u8>) -> [u8; 16] {
    let cipher = Aes256Gcm.create(key)!;
    let tag = cipher.seal([0u8; 12], "".bytes(), data);
    unsafe { wipe_check_sink(tag[0] as u64); } // after the schedule's last use, before the cipher's drop
    return tag;
}

fn main() {
    var key = [0u8; 32];
    let _ = Random.fill(mut key[..]);
    var data = [1u8; 64];
    let tag = sealOnce(key[..], mut data[..]);
    Console.writeLine(tag.length);
}
"""

WIPE = "Secret.wipe(mut this.bytes);"
LOOP = "for (i in 0..32) { this.bytes[i] = 0; }"

LEVELS = [("-O0", ["-O0"]), ("-O2", ["-O2"]), ("-O3", ["-O3"]), ("-O2 -flto", ["-O2", "-flto"])]
HEADER = re.compile(r"^[0-9a-f]+ <(.+)>:\s*$")
# A call, or a tail call, to a function's start: a branch to `<f+0x…>` is a jump inside a function, not a call.
CALL = re.compile(r"\b(?:bl|call|callq|jmp|b)\s+(?:0x)?[0-9a-f]+\s*<([^>+]+)>")
ZEROING = re.compile(r"\bst(?:r|p|ur)[a-z]*\s+(?:[xw]zr|.*,\s*[xw]zr)|\bmovi?\s+v\d+\.\S+,\s*#0|\bxorps?|"
                     r"\bpxor|\bmovq?\s+\$0x0,|\bmovl?\s+\$0x0,")


def bare(name):
    """A function's name as the source has it: without the leading `_` of Mach-O, and without the suffix of a clone gcc
    makes (`ceesafe_wipe.constprop.0` under -O2 -flto, its length folded in; `.isra.N`, `.part.N`, `.lto_priv.N`,
    `.cold`), which is the same function."""
    name = name[1:] if name.startswith("_") else name
    return re.sub(r"(\.(constprop|isra|part|lto_priv|cold)(\.\d+)?)+$", "", name)


def compilers():
    """The C compilers to build with: clang, and gcc when it is GCC (macOS's gcc is clang)."""
    found = []
    clang = shutil.which("clang")
    if clang:
        found.append(("clang", clang))
    for name in ("gcc", "gcc-15", "gcc-14", "gcc-13", "gcc-12"):
        path = shutil.which(name)
        if not path:
            continue
        version = subprocess.run([path, "--version"], capture_output=True, text=True).stdout
        if "clang" not in version.lower() and "Free Software Foundation" in version:
            found.append((name, path))
            break
    return found


def disassemble(objdump, exe):
    """Each function's instructions, by bare name."""
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", exe], capture_output=True, text=True).stdout
    functions, current = {}, None
    for line in out.splitlines():
        m = HEADER.match(line)
        if m:
            current = bare(m.group(1))
            functions.setdefault(current, [])
        elif current is not None and line.strip():
            functions[current].append(line)
    return functions


def callee(line):
    m = CALL.search(line)
    return bare(m.group(1)) if m else None


def reaches(functions, name, target, depth, seen=None):
    if name == target:
        return True
    if depth == 0 or name not in functions:
        return False
    seen = set() if seen is None else seen
    if name in seen:
        return False
    seen.add(name)
    for line in functions[name]:
        c = callee(line)
        if c is not None and c != name and reaches(functions, c, target, depth - 1, seen):
            return True
    return False


def after_last_use(functions, anchor):
    """(wiped, zeroing) over the functions that use the secret: whether one calls into ceesafe_wipe after its last
    call that reaches `anchor`, and whether any zeroing follows that call."""
    wiped, zeroing, used = False, False, False
    for name, lines in functions.items():
        if name in (anchor, "ceesafe_wipe") or name.startswith("__stubs"):
            continue
        last = None
        for k, line in enumerate(lines):
            c = callee(line)
            if c is not None and c != name and reaches(functions, c, anchor, 3):
                last = k
        if last is None:
            continue
        used = True
        for line in lines[last + 1:]:
            c = callee(line)
            if c is not None and reaches(functions, c, "ceesafe_wipe", 3):
                wiped = True
            if c in ("bzero", "memset", "__bzero", "explicit_bzero", "memset_s") or ZEROING.search(line):
                zeroing = True
    return used, wiped, zeroing


def build(compiler, cc, flags, sources, exe):
    argv = [compiler, "build"] + sources + ["-o", exe, "--cc", cc]
    for flag in flags:
        argv += ["--cflag", flag]
    return subprocess.run(argv, capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--objdump", default=shutil.which("llvm-objdump") or shutil.which("objdump"))
    args = parser.parse_args()
    if not args.objdump:
        print("wipe_check: no objdump")
        return 1
    compiler = os.path.abspath(args.compiler)
    anchor = "wipe_check_sink"
    programs = [("key", KEY.replace("WIPE", WIPE), anchor, True),
                ("aes", AES, anchor, True),
                ("key without Secret.wipe", KEY.replace("WIPE", LOOP), anchor, False)]
    ccs = compilers()
    problems, rows = 0, 0
    with tempfile.TemporaryDirectory(prefix="ceesafe-wipe-") as work:
        sink = os.path.join(work, "sink.c")
        with open(sink, "w", encoding="utf-8", newline="\n") as f:
            f.write(SINK)
        for name, text, anchor, wipes in programs:
            source = os.path.join(work, name.split()[0] + ("" if wipes else "-loop") + ".cee")
            with open(source, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            for cc_name, cc in ccs:
                for level, flags in LEVELS:
                    rows += 1
                    exe = source[:-4] + "-" + cc_name + level.replace(" ", "").replace("-", "_")
                    built = build(compiler, cc, flags, [source, sink], exe)
                    where = f"{name}, {cc_name} {level}"
                    if built.returncode != 0:
                        problems += 1
                        print(f"wipe_check: {where}: the build failed:\n{built.stderr.strip()[:800]}")
                        continue
                    ran = subprocess.run([exe], capture_output=True, text=True, timeout=60)
                    if ran.returncode != 0:
                        problems += 1
                        print(f"wipe_check: {where}: exit status {ran.returncode}")
                        continue
                    used, wiped, zeroing = after_last_use(disassemble(args.objdump, exe), anchor)
                    if not used:
                        problems += 1
                        print(f"wipe_check: {where}: no function calls {anchor}: the analysis found no last use")
                    elif wipes and not wiped:
                        problems += 1
                        print(f"wipe_check: {where}: ceesafe_wipe is not called after the last use")
                    elif not wipes and wiped:
                        problems += 1
                        print(f"wipe_check: {where}: the control reaches ceesafe_wipe: the check would pass vacuously")
                    elif wipes:
                        print(f"wipe_check: {where}: ceesafe_wipe called after the last use")
                    else:
                        kept = "zeroing kept" if zeroing else "the stores are gone"
                        print(f"wipe_check: {where}: control: {kept}"
                              + (" (reported: the compiler kept them)" if zeroing and level != "-O0" else ""))
    print(f"wipe_check: {len(programs)} programs, {' and '.join(n for n, _ in ccs) or 'no C compiler'}, "
          f"{len(LEVELS)} levels: {rows} builds, {problems} problems")
    return 1 if problems or not ccs else 0


if __name__ == "__main__":
    sys.exit(main())
