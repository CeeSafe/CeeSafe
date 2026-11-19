#!/usr/bin/env python3
"""Printing a PDF (ADR 0178): FileDialog.printFile, scripted on every system and, on Windows, printed for real.

One program is built. It opens a window and gives printFile, in turn, a PDF of two pages (a portrait one and a landscape
one), a file that is not there, and a file that is not a PDF, printing what each came to:

1. with a scripted window (CEESAFE_WINDOW_SCRIPT), the script's `dialog` items answer in order: one with a path is
   printed, `dialog` alone a cancel, and a dialog with no item left is cancelled; the files are not read;
2. on Windows (ADR 0178 §6), with CEESAFE_PRINT_PRINTER naming Microsoft Print to PDF and CEESAFE_PRINT_OUTPUT a file,
   the dialog is skipped: the PDF is loaded and rendered by Windows.Data.Pdf and printed, and the printer writes a PDF
   of two pages; a missing file and a file that is not a PDF are Resources, and nothing is printed for them.

The other systems' print dialogs need a person (ADR 0178 §5).

usage: python3 tools/print_check.py --compiler out/ceesafec
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

PROGRAM = r'''fn say(what: Result<bool, WindowError>) {
    Console.writeLine(match (what) {
        Ok(true) => "printed",
        Ok(false) => "cancelled",
        Err(WindowError.Unsupported) => "unsupported",
        Err(WindowError.Resources) => "resources",
        Err(_) => "another error",
    });
    Console.flush();
}

fn main() {
    var window = match (Window.open("print_check", 64, 48)) {
        Ok(w) => w,
        Err(_) => {
            Console.writeLine("no window");
            return;
        }
    };
    var frame = Vector<u32>.create();
    for (k in 0u32..(64 * 48)) {
        let _ = k;
        frame.push(0xff808080)!;
    }
    let _ = window.present(frame.view(), 64, 48);
    window.waitFrame();
    say(FileDialog.printFile(mut window, "print_check", "@PDF@"));
    say(FileDialog.printFile(mut window, "print_check", "@MISSING@"));
    say(FileDialog.printFile(mut window, "print_check", "@TEXT@"));
}
'''


def pdf(pages):
    """A PDF of `pages`, each (width, height) in points, with a line of text on each: the catalog, the page tree and
    the font, then each page and its content."""
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>", None, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
    kids = []
    for k, (width, height) in enumerate(pages):
        page = len(objects) + 1
        kids.append(b"%d 0 R" % page)
        objects.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Contents %d 0 R "
                       b"/Resources << /Font << /F1 3 0 R >> >> >>" % (width, height, page + 1))
        text = b"BT /F1 24 Tf 72 %d Td (print_check page %d) Tj ET" % (height - 96, k + 1)
        objects.append(b"<< /Length %d >>\nstream\n%s\nendstream" % (len(text), text))
    objects[1] = b"<< /Type /Pages /Kids [%s] /Count %d >>" % (b" ".join(kids), len(pages))
    out = b"%PDF-1.4\n"
    offsets = []
    for k, body in enumerate(objects):
        offsets.append(len(out))
        out += b"%d 0 obj\n%s\nendobj\n" % (k + 1, body)
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    out += b"".join(b"%010d 00000 n \n" % o for o in offsets)
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objects) + 1, xref)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    cases = 0
    with tempfile.TemporaryDirectory() as work:
        document = os.path.join(work, "two pages.pdf")
        text = os.path.join(work, "not a pdf.pdf")
        with open(document, "wb") as f:
            f.write(pdf([(595, 842), (842, 595)]))
        with open(text, "w") as f:
            f.write("not a PDF\n")
        slash = lambda p: p.replace("\\", "/")
        source = os.path.join(work, "main.cee")
        exe = os.path.join(work, "print.exe" if os.name == "nt" else "print")
        with open(source, "w", encoding="utf-8") as f:
            f.write(PROGRAM.replace("@PDF@", slash(document)).replace("@MISSING@", slash(os.path.join(work, "none.pdf")))
                    .replace("@TEXT@", slash(text)))
        built = subprocess.run([compiler, "build", "--windowed", source, "-o", exe], capture_output=True, text=True)
        if built.returncode != 0:
            print(f"FAIL build: {built.stderr.strip()[:800]}")
            print("print_check: 0 cases: 1 problems")
            return 1

        def run(env_extra, label):
            env = dict(os.environ)
            for name in ("CEESAFE_WINDOW_SCRIPT", "CEESAFE_WINDOW_DUMP", "CEESAFE_PRINT_PRINTER", "CEESAFE_PRINT_OUTPUT"):
                env.pop(name, None)
            env.update(env_extra)
            result = subprocess.run([exe], capture_output=True, env=env, timeout=300)
            out = result.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
            if result.returncode != 0:
                problems.append(f"{label}: exit {result.returncode}: {result.stderr.decode('utf-8', 'replace')[:400]}")
            return out

        cases += 1
        script = os.path.join(work, "script.txt")
        with open(script, "w") as f:
            f.write("0:dialog:printed;0:dialog")
        out = run({"CEESAFE_WINDOW_SCRIPT": script}, "scripted")
        if out != "printed\ncancelled\ncancelled\n":
            problems.append(f"scripted: printed\n{out}\nnot\nprinted\ncancelled\ncancelled")

        if sys.platform == "win32":
            cases += 1
            output = os.path.join(work, "printed.pdf")
            out = run({"CEESAFE_PRINT_PRINTER": "Microsoft Print to PDF", "CEESAFE_PRINT_OUTPUT": output}, "Windows")
            if out != "printed\nresources\nresources\n":
                problems.append(f"Windows: printed\n{out}\nnot\nprinted\nresources\nresources")
            written = b""
            if os.path.exists(output):
                with open(output, "rb") as f:
                    written = f.read()
            pages = len(re.findall(rb"/Type\s*/Page(?!s)", written))
            if not written.startswith(b"%PDF-") or pages != 2:
                problems.append(f"Windows: Microsoft Print to PDF wrote {len(written)} bytes, {pages} pages, not a PDF "
                                f"of 2")
    for p in problems:
        print(f"FAIL {p}")
    print(f"print_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
