#!/usr/bin/env python3
"""Writes the Android window's Java class into runtime/hosted/ceesafe_rt_window.c as an embedded dex (ADR 0157).

runtime/android/org/ceesafe/window/InputView.java (ADR 0157), Picker.java (ADR 0158), AccessView.java (ADR 0164),
Bluetooth.java (ADR 0177), Printing.java (ADR 0178) and Secrets.java (ADR 0242) are the sources. They are compiled with javac against the Android
SDK's android.jar and turned into one classes.dex by the SDK's d8 (release mode, minimum API 25, ADR 0188's floor); the bytes
replace the array between the two marker lines of the runtime's window file. The array is committed, so building a
program needs neither a JDK nor the SDK: run this only when the Java source changes, then tools/gen_runtime.py.

The array's first line records the SHA-256 of the Java sources it was made from. --check needs no tool: it fails when
that is not the source's hash now, which is how the contract suite knows the dex was made again after the source
changed (the bytes themselves differ from one javac or d8 to the next, so they are not compared).

usage: python3 tools/gen_android_dex.py [--check]
       ANDROID_HOME (default ~/Library/Android/sdk, then ~/Android/Sdk) names the SDK; the newest platform and
       build-tools in it are used.
"""

import argparse
import glob
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SOURCES = [os.path.join(ROOT, "runtime", "android", "org", "ceesafe", "window", name)
           for name in ("InputView.java", "Picker.java", "AccessView.java", "Bluetooth.java", "Printing.java",
                        "Secrets.java")]
TARGET = os.path.join(ROOT, "runtime", "hosted", "ceesafe_rt_window.c")
BEGIN = "/* BEGIN android dex: written by tools/gen_android_dex.py from runtime/android; do not edit */\n"
END = "/* END android dex */\n"


def newest(pattern):
    def version(path):
        return [int(p) if p.isdigit() else -1 for p in os.path.basename(os.path.dirname(path)).replace("android-", "").split(".")]

    found = sorted(glob.glob(pattern), key=version)
    return found[-1] if found else None


def sdk():
    for path in (os.environ.get("ANDROID_HOME"), os.path.expanduser("~/Library/Android/sdk"),
                 os.path.expanduser("~/Android/Sdk")):
        if path and os.path.isdir(path):
            return path
    sys.exit("gen_android_dex: no Android SDK (set ANDROID_HOME)")


def dex():
    home = sdk()
    jar = newest(os.path.join(home, "platforms", "android-*", "android.jar"))
    d8 = newest(os.path.join(home, "build-tools", "*", "d8"))
    if jar is None or d8 is None:
        sys.exit("gen_android_dex: the SDK has no platform's android.jar or no build-tools' d8")
    with tempfile.TemporaryDirectory() as work:
        classes = os.path.join(work, "classes")
        os.makedirs(classes)
        subprocess.run(["javac", "--release", "11", "-Xlint:-options", "-g:none", "-classpath", jar, "-d", classes,
                        ] + SOURCES, check=True)
        found = sorted(glob.glob(os.path.join(classes, "**", "*.class"), recursive=True))
        subprocess.run([d8, "--release", "--min-api", "25", "--lib", jar, "--output", work] + found, check=True)
        with open(os.path.join(work, "classes.dex"), "rb") as f:
            return f.read()


def source_hash():
    digest = hashlib.sha256()
    for source in SOURCES:
        with open(source, "rb") as f:
            digest.update(f.read().replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def array(data):
    lines = [BEGIN, f"/* the Java sources' SHA-256: {source_hash()} */\n",
             f"static uint8_t cee_android_dex[{len(data)}] = {{\n"]
    for k in range(0, len(data), 24):
        lines.append("    " + ",".join(str(b) for b in data[k:k + 24]) + ",\n")
    lines += ["};\n", END]
    return "".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    with open(TARGET, encoding="utf-8") as f:
        text = f.read()
    start, end = text.find(BEGIN), text.find(END)
    if start < 0 or end < start:
        sys.exit("gen_android_dex: runtime/hosted/ceesafe_rt_window.c has no android dex markers")
    if args.check:
        if f"/* the Java sources' SHA-256: {source_hash()} */\n" not in text[start:end]:
            sys.exit("gen_android_dex: the embedded dex was not made from runtime/android's Java sources as they are "
                     "now: run tools/gen_android_dex.py, then tools/gen_runtime.py")
        print("gen_android_dex: the embedded dex was made from runtime/android's Java sources as they are now; 0 problems")
        return
    written = text[:start] + array(dex()) + text[end + len(END):]
    with open(TARGET, "w", encoding="utf-8", newline="") as f:
        f.write(written)
    print(f"gen_android_dex: wrote the dex into {TARGET}")


if __name__ == "__main__":
    main()
