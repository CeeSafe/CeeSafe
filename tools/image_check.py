#!/usr/bin/env python3
"""Making a photo smaller (ADR 0266): PhotoFile.shrink, natively and interpreted.

One program is built; its arguments are the source, the target, the longest side and the quality. Its output is
`made W H`, or the IoError's name. Each case is run:

1. on macOS (ImageIO): a 4032 by 3024 JPEG whose EXIF orientation is 6 (turned a quarter) becomes a JPEG of at most
   1600 pixels, upright: 1200 by 1600, with no EXIF (Pillow reads it back: no orientation, no GPS, no camera); a
   small PNG is written as a JPEG at its own size (never enlarged); a file that is no photo is InvalidInput; a missing
   one NotFound; a longest side of 0 is InvalidInput;
2. on Linux and FreeBSD: Unsupported, for every case;
3. under `ceesafec run --interp`: Unsupported.

Pillow makes the fixtures and reads the results; without it the check is skipped (exit 0 with a note).

usage: python3 tools/image_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile

PROGRAM = r'''fn main() -> i32 {
    let from = Process.argument(1) ?? "";
    let to = Process.argument(2) ?? "";
    let text = Process.argument(3) ?? "0";
    let longest = truncate<u32>(text.parseInt<u64>() ?? 0);
    match (PhotoFile.shrink(from, to, longest, 0.82)) {
        Ok(size) => {
            Console.write("made ");
            Console.write(size.w);
            Console.write(" ");
            Console.writeLine(size.h);
        }
        Err(IoError.Unsupported) => { Console.writeLine("Unsupported"); }
        Err(IoError.NotFound) => { Console.writeLine("NotFound"); }
        Err(IoError.InvalidInput) => { Console.writeLine("InvalidInput"); }
        Err(_) => { Console.writeLine("another error"); }
    }
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    compiler = os.path.abspath(parser.parse_args().compiler)
    try:
        from PIL import Image
    except ImportError:
        print("image_check: Pillow is not installed: skipped")
        return 0
    problems = []
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "shrink.cee")
        with open(source, "w", encoding="utf-8") as f:
            f.write(PROGRAM)
        exe = os.path.join(work, "shrink")
        r = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if r.returncode != 0:
            print("image_check: the program did not build\n" + r.stdout + r.stderr)
            return 1
        # A camera's photo, stored on its side (orientation 6), with a place and a camera in its EXIF.
        photo = os.path.join(work, "photo.jpg")
        stored = Image.new("RGB", (4032, 3024), (200, 40, 40))
        for x in range(0, 400):
            for y in range(0, 300):
                stored.putpixel((x, y), (20, 200, 20))  # a green corner: where it goes shows the turn
        exif = Image.Exif()
        exif[0x0112] = 6  # Orientation
        exif[0x010F] = "CameraMaker"  # Make
        stored.save(photo, "JPEG", quality=92, exif=exif)
        small = os.path.join(work, "small.png")
        Image.new("RGB", (300, 200), (10, 10, 240)).save(small, "PNG")
        junk = os.path.join(work, "junk.jpg")
        with open(junk, "wb") as f:
            f.write(b"this is not a photo at all" * 40)
        apple = sys.platform == "darwin"

        def shrink(src, longest, name, interp=False):
            out = os.path.join(work, name)
            command = ([compiler, "run", "--interp", source, "--", src, out, str(longest)] if interp
                       else [exe, src, out, str(longest)])
            r = subprocess.run(command, capture_output=True, text=True)
            return r.stdout.strip(), out

        got, out = shrink(photo, 1600, "made.jpg")
        if apple:
            if got != "made 1200 1600":
                problems.append(f"a 4032 by 3024 photo turned a quarter: expected 'made 1200 1600', got {got!r}")
            else:
                made = Image.open(out)
                if made.format != "JPEG" or made.size != (1200, 1600):
                    problems.append(f"the result is {made.format} {made.size}, not a JPEG of 1200 by 1600")
                kept = made.getexif()
                if kept.get(0x0112, 1) != 1 or 0x010F in kept or 0x8825 in kept:
                    problems.append(f"the result kept the photo's metadata: {dict(kept)}")
                # The green corner was the stored top left; turned a quarter clockwise it is the top right.
                r_, g_, b_ = made.convert("RGB").getpixel((1200 - 20, 20))
                if not (g_ > 150 and r_ < 100):
                    problems.append(f"the photo is not upright: the top right is {(r_, g_, b_)}, not green")
        elif got != "Unsupported":
            problems.append(f"no codec on this system: expected Unsupported, got {got!r}")
        got, out = shrink(small, 1600, "small.jpg")
        want = "made 300 200" if apple else "Unsupported"
        if got != want:
            problems.append(f"a small PNG: expected {want!r} (never enlarged), got {got!r}")
        elif apple and Image.open(out).format != "JPEG":
            problems.append("a small PNG was not written as a JPEG")
        for what, src, longest, want in [("no photo", junk, 1600, "InvalidInput"),
                                         ("a missing file", os.path.join(work, "none.jpg"), 1600, "NotFound"),
                                         ("a longest side of 0", photo, 0, "InvalidInput")]:
            got, _ = shrink(src, longest, "other.jpg")
            expected = want if apple or want != "InvalidInput" or longest == 0 else "Unsupported"
            if not apple and want == "NotFound":
                expected = "NotFound"
            if got != expected:
                problems.append(f"{what}: expected {expected!r}, got {got!r}")
        got, _ = shrink(photo, 1600, "interp.jpg", interp=True)
        if got != "Unsupported":
            problems.append(f"the interpreter: expected Unsupported, got {got!r}")
    for p in problems:
        print("FAIL " + p)
    print(f"image_check: 7 cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
