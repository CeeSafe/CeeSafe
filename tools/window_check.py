#!/usr/bin/env python3
"""Checks of lib/std's Window (type-system.md §26.13, ADR 0069) that the corpus cannot make, since a corpus program
that opened a window would open one on the developer's screen:

1. with CEESAFE_WINDOW_SCRIPT, a program that prints every event it gets, frame by frame, and presents a pattern of
   each frame's size and number: its output must equal the trace this script computes from the same script by the
   grammar of §26.13 (click's up on the next frame, key's down then up, one Text per character, a resize or focus
   only when it changes something, delivery at the frame after the n-th present), and CEESAFE_WINDOW_DUMP must hold
   the last frame, byte for byte;
2. the errors: a malformed script and a missing one are Resources, a zero size is Unsupported, an open on a thread
   other than the first is NotMainThread, and the interpreter answers Unsupported;
3. the surfaces (ADR 0137): a scripted window's nativeHandle is Scripted, a Frames window presents, an External
   one answers Unsupported to present and presentChanged and dumps no frame, and the interpreter answers Unsupported
   to openWith; the script's `scale` item gives Resized only when it changes the scale (in the trace of 1);
4. with --display (a machine with a window server, not CI), a real window opens, reports its size and a scale of at
   least 1, takes frames and closes; and an External one gives this system's handles, not zero.

usage: python3 tools/window_check.py --compiler out/ceesafec [--display]
"""

import argparse
import os
import subprocess
import sys
import tempfile

EVENTS = r"""
fn key(k: WindowKey) {
    match (k) {
        WindowKey.Tab => { Console.write("tab"); }
        WindowKey.Enter => { Console.write("enter"); }
        WindowKey.Escape => { Console.write("escape"); }
        WindowKey.Backspace => { Console.write("backspace"); }
        WindowKey.Delete => { Console.write("delete"); }
        WindowKey.Left => { Console.write("left"); }
        WindowKey.Right => { Console.write("right"); }
        WindowKey.Up => { Console.write("up"); }
        WindowKey.Down => { Console.write("down"); }
        WindowKey.Home => { Console.write("home"); }
        WindowKey.End => { Console.write("end"); }
        WindowKey.PageUp => { Console.write("pageup"); }
        WindowKey.PageDown => { Console.write("pagedown"); }
        WindowKey.Space => { Console.write("space"); }
        WindowKey.Character(c) => { Console.write("char:"); Console.write(c); }
        WindowKey.Function(n) => { Console.write("f"); Console.write(n); }
        WindowKey.Other(c) => { Console.write("other:"); Console.write(c); }
    }
}

fn bit(b: bool) -> u32 => if (b) 1 else 0;

/// Prints one event; true for Closed.
fn show(e: WindowEvent) -> bool {
    match (e) {
        WindowEvent.Closed => { Console.writeLine("closed"); return true; }
        WindowEvent.Resized(s) => { Console.write("resized "); Console.write(s.width); Console.write(" "); Console.writeLine(s.height); }
        WindowEvent.Focus(f) => { Console.write("focus "); Console.writeLine(bit(f)); }
        WindowEvent.PointerMoved(x, y) => { Console.write("move "); Console.write(x); Console.write(" "); Console.writeLine(y); }
        WindowEvent.PointerButton(b, down, x, y) => {
            Console.write("button ");
            Console.write(match (b) { WindowButton.Primary => 0u32, WindowButton.Secondary => 1, WindowButton.Middle => 2 });
            Console.write(" ");
            Console.write(bit(down));
            Console.write(" ");
            Console.write(x);
            Console.write(" ");
            Console.writeLine(y);
        }
        WindowEvent.Wheel(dx, dy) => { Console.write("wheel "); Console.write(dx); Console.write(" "); Console.writeLine(dy); }
        WindowEvent.Key(k, down, repeat, m) => {
            Console.write("key ");
            key(k);
            Console.write(" ");
            Console.write(bit(down));
            Console.write(" ");
            Console.write(bit(repeat));
            Console.write(" ");
            Console.writeLine(bit(m.shift) + 2 * bit(m.control) + 4 * bit(m.alt) + 8 * bit(m.command));
        }
        WindowEvent.Text(c) => { Console.write("text "); Console.writeLine(c); }
    }
    return false;
}

fn main() -> i32 {
    var window = match (Window.open("window_check", 40, 30)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    window.setTitle("window_check frames");
    window.setCursor(WindowCursor.Pointer);
    var pixels = Vector<u32>.create();
    for (frame in 0u32..16) {
        Console.write("frame ");
        Console.writeLine(frame);
        var closed = false;
        loop {
            match (window.nextEvent()) {
                Some(e) => { if (show(e)) { closed = true; } }
                null => { break; }
            }
        }
        if (closed) {
            break;
        }
        let size = window.size();
        pixels.clear();
        for (y in 0..size.height) {
            for (x in 0..size.width) {
                pixels.push(0xff000000u32 | (x & 0xff) | (y & 0xff) << 8 | (frame & 0xff) << 16)!;
            }
        }
        window.present(pixels.view(), size.width, size.height)!;
        window.waitFrame();
    }
    Console.write("scale ");
    Console.writeLine(window.scale());
    return 0;
}
"""

ERRORS = r"""
fn openCode(width: u32) -> u32 => match (Window.open("window_check", width, 20)) {
    Ok(_) => 0,
    Err(WindowError.Unsupported) => 1,
    Err(WindowError.NoDisplay) => 2,
    Err(WindowError.NotMainThread) => 3,
    Err(WindowError.Resources) => 4,
};

fn main() {
    let mode = match (Process.argument(1)) {
        Some(m) => m,
        null => "open",
    };
    if (mode == "thread") {
        let t = Thread.spawn(move () => openCode(20))!;
        Console.writeLine(t.join());
    } else if (mode == "zero") {
        Console.writeLine(openCode(0));
    } else {
        Console.writeLine(openCode(20));
    }
}
"""

# Surfaces and handles (ADR 0137): prints the handle's kind, then what present and presentChanged answer.
SURFACE = r"""
fn kind(h: WindowHandle) -> u32 => match (h) {
    WindowHandle.Scripted => 0,
    WindowHandle.AppKit(a, b) => if (a != 0 && b != 0) 1 else 9,
    WindowHandle.Win32(a, b) => if (a != 0 && b != 0) 2 else 9,
    WindowHandle.Xlib(a, b) => if (a != 0 && b != 0) 3 else 9,
};

fn code(r: Result<void, WindowError>) -> u32 => match (r) {
    Ok() => 0,
    Err(WindowError.Unsupported) => 1,
    Err(WindowError.NoDisplay) => 2,
    Err(WindowError.NotMainThread) => 3,
    Err(WindowError.Resources) => 4,
};

fn main() -> i32 {
    let mode = match (Process.argument(1)) {
        Some(m) => m,
        null => "frames",
    };
    let surface = if (mode == "external") WindowSurface.External else WindowSurface.Frames;
    var window = match (Window.openWith("window_check surface", 8, 6, surface)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    let pixels = [0xff0000ffu32; 48];
    let one = [WindowBox { x: 1, y: 1, width: 2, height: 2 }];
    Console.write(kind(window.nativeHandle()));
    Console.write(" ");
    Console.write(code(window.present(pixels[..], 8, 6)));
    Console.write(" ");
    Console.writeLine(code(window.presentChanged(pixels[..], 8, 6, one[..])));
    return 0;
}
"""

# presentChanged (ADR 0070): step k paints the whole image color k but says only its boxes changed. A scripted window
# applies only the boxes, so the last dump shows which ones it applied.
CHANGED = r"""
fn color(k: u32) -> u32 => 0xff000000u32 | (k * 37 + 11) & 0xff | ((k * 59 + 7) & 0xff) << 8 | ((k * 83 + 3) & 0xff) << 16;

fn main() -> i32 {
    var window = match (Window.open("window_check changed", 16, 12)) {
        Ok(w) => w,
        Err(_) => { return 1; }
    };
    let steps = match (Process.argument(1)) {
        Some(a) => if (a.length == 1) truncate<u32>(a.bytes()[0] - 48) else 0,
        null => 0,
    };
    var pixels = Vector<u32>.create();
    for (k in 0..steps) {
        let width: u32 = if (k >= 5) 20 else 16;
        let height: u32 = if (k >= 5) 14 else 12;
        pixels.clear();
        for (i in 0..(width * height) as usize) {
            pixels.push(color(k))!;
        }
        let one = [WindowBox { x: 2, y: 2, width: 4, height: 4 }];
        let edges = [WindowBox { x: 0, y: 0, width: 16, height: 1 }, WindowBox { x: 15, y: 0, width: 1, height: 12 }];
        let outside = [WindowBox { x: 10, y: 10, width: 10, height: 10 }];
        let late = [WindowBox { x: 1, y: 1, width: 2, height: 2 }];
        let tiny = [WindowBox { x: 3, y: 3, width: 0, height: 5 }, WindowBox { x: 1, y: 1, width: 1, height: 1 }];
        let result = match (k) {
            0 => window.presentChanged(pixels.view(), width, height, one[..]),
            1 => window.presentChanged(pixels.view(), width, height, one[..]),
            2 => window.presentChanged(pixels.view(), width, height, one[0..0]),
            3 => window.presentChanged(pixels.view(), width, height, edges[..]),
            4 => window.presentChanged(pixels.view(), width, height, outside[..]),
            5 => window.presentChanged(pixels.view(), width, height, late[..]),
            6 => window.present(pixels.view(), width, height),
            _ => window.presentChanged(pixels.view(), width, height, tiny[..]),
        };
        match (result) {
            Ok() => {}
            Err(_) => { return 2; }
        }
    }
    return 0;
}
"""


def color(k):
    return (0xFF000000 | ((k * 37 + 11) & 0xFF) | (((k * 59 + 7) & 0xFF) << 8) | (((k * 83 + 3) & 0xFF) << 16))


def changed_expected(steps):
    """The frame a scripted window shows after `steps` steps of CHANGED, by ADR 0070's rules."""
    boxes = {0: [(2, 2, 4, 4)], 1: [(2, 2, 4, 4)], 2: [], 3: [(0, 0, 16, 1), (15, 0, 1, 12)], 4: [(10, 10, 10, 10)],
             5: [(1, 1, 2, 2)], 7: [(3, 3, 0, 5), (1, 1, 1, 1)]}
    frame, size = None, None
    for k in range(steps):
        w, h = (20, 14) if k >= 5 else (16, 12)
        full = k == 6 or frame is None or size != (w, h) or any(
            bw and bh and (x + bw > w or y + bh > h) for x, y, bw, bh in boxes[k])
        if full:
            frame, size = [color(k)] * (w * h), (w, h)
            continue
        for x, y, bw, bh in boxes[k]:
            for yy in range(y, y + bh):
                for xx in range(x, x + bw):
                    frame[yy * w + xx] = color(k)
    w, h = size
    out = bytearray(f"ceesafe-window {w} {h}\n".encode())
    for p in frame:
        out += bytes((p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, p >> 24))
    return bytes(out)


DISPLAY = r"""
fn main() -> i32 {
    var window = match (Window.open("window_check --display", 320, 200)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    var pixels = Vector<u32>.create();
    for (frame in 0u32..30) {
        loop {
            match (window.nextEvent()) {
                Some(_) => {}
                null => { break; }
            }
        }
        let size = window.size();
        let scale = window.scale();
        let w = saturatingTruncate<u32>(round<f32>(size.width) * scale);
        let h = saturatingTruncate<u32>(round<f32>(size.height) * scale);
        pixels.clear();
        for (i in 0..(w as u64) * (h as u64)) {
            pixels.push(0xff000000u32 | (frame * 8) << 8 | truncate<u32>(i & 0xff))!;
        }
        // Every other frame through presentChanged: the first row band changed, then everything (ADR 0070).
        if (frame % 2 == 0) {
            window.present(pixels.view(), w, h)!;
        } else {
            let band = [WindowBox { x: 0, y: 0, width: w, height: h / 4 }];
            window.presentChanged(pixels.view(), w, h, band[..])!;
        }
        window.waitFrame();
    }
    let size = window.size();
    Console.write(size.width);
    Console.write(" ");
    Console.write(size.height);
    Console.write(" ");
    Console.writeLine(if (window.scale() >= 1.0) "scaled" else "unscaled");
    return 0;
}
"""

# Every kind of item, a click across frames, a resize that changes nothing, focus that changes nothing, a scale that
# changes nothing and one that does (Resized at the same size, ADR 0137), non-ASCII text, and a close that ends the run.
SCRIPT = ("0:move:10,20;0:click:5,6;1:key:a;1:keydown:f5;1:keyup:f5;1:type:Hé✓;2:wheel:0,-1;2:wheel:1.5,0;"
          "3:down:1,7,8;3:up:2,9,10;4:resize:40,30;4:resize:32,24;4:scale:1;5:focus:1;5:focus:0;5:scale:2;5:scale:2;"
          "5:key:tab;6:key:enter;"
          "6:key:pagedown;6:key:f12;6:key:Z;7:type:ok;8:close;9:move:1,1")

NAMED = {"tab": "tab", "enter": "enter", "escape": "escape", "backspace": "backspace", "delete": "delete",
         "left": "left", "right": "right", "up": "up", "down": "down", "home": "home", "end": "end",
         "pageup": "pageup", "pagedown": "pagedown", "space": "space"}


def number(text):
    """Console.write of an f32: an integer without a point, otherwise its shortest decimal (both are exact here)."""
    v = float(text)
    return str(int(v)) if v == int(v) else repr(v)


def key_name(name):
    if name in NAMED:
        return NAMED[name]
    if len(name) in (2, 3) and name[0] == "f" and name[1:].isdigit() and 1 <= int(name[1:]) <= 12:
        return "f" + name[1:]
    assert len(name) == 1
    return "char:" + str(ord(name.lower()))


def expected(script, width, height):
    """The trace and the last frame (size and number) of EVENTS run with `script`, by §26.13's grammar."""
    events = []  # (frame, order, line or ("resize", w, h) or ("focus", f))
    for order, item in enumerate(i for i in script.split(";") if i):
        frame, what, *rest = item.split(":", 2)
        frame = int(frame)
        args = rest[0] if rest else ""
        nums = args.split(",") if args else []
        if what == "move":
            events.append((frame, order, f"move {number(nums[0])} {number(nums[1])}"))
        elif what in ("down", "up"):
            down = 1 if what == "down" else 0
            events.append((frame, order, f"button {int(nums[0])} {down} {number(nums[1])} {number(nums[2])}"))
        elif what == "click":
            events.append((frame, order, f"button 0 1 {number(nums[0])} {number(nums[1])}"))
            events.append((frame + 1, order, f"button 0 0 {number(nums[0])} {number(nums[1])}"))
        elif what == "wheel":
            events.append((frame, order, f"wheel {number(nums[0])} {number(nums[1])}"))
        elif what in ("key", "keydown", "keyup"):
            name = key_name(args)
            if what != "keyup":
                events.append((frame, order, f"key {name} 1 0 0"))
            if what != "keydown":
                events.append((frame, order, f"key {name} 0 0 0"))
        elif what == "type":
            for ch in args:
                events.append((frame, order, f"text {ord(ch)}"))
        elif what == "resize":
            events.append((frame, order, ("resize", int(nums[0]), int(nums[1]))))
        elif what == "focus":
            events.append((frame, order, ("focus", int(args))))
        elif what == "scale":
            events.append((frame, order, ("scale", float(args))))
        elif what == "close":
            events.append((frame, order, "closed"))
        else:
            raise ValueError(item)
    events.sort(key=lambda e: e[0])  # stable: the script's order within a frame
    lines, size, focused, scale, last = [], (width, height), 1, 1.0, None
    for frame in range(16):
        lines.append(f"frame {frame}")
        closed = False
        for f, _, e in events:
            if f != frame:
                continue
            if isinstance(e, tuple) and e[0] == "resize":
                if (e[1], e[2]) != size:
                    size = (e[1], e[2])
                    lines.append(f"resized {size[0]} {size[1]}")
            elif isinstance(e, tuple) and e[0] == "scale":
                if e[1] != scale:
                    scale = e[1]
                    lines.append(f"resized {size[0]} {size[1]}")
            elif isinstance(e, tuple):
                if e[1] != focused:
                    focused = e[1]
                    lines.append(f"focus {focused}")
            else:
                lines.append(e)
                closed = closed or e == "closed"
        if closed:
            break
        last = (size, frame)
    lines.append(f"scale {number(str(scale))}")
    return lines, last


def frame_bytes(size, frame):
    width, height = size
    out = bytearray(f"ceesafe-window {width} {height}\n".encode())
    for y in range(height):
        for x in range(width):
            out += bytes((x & 0xFF, y & 0xFF, frame & 0xFF, 0xFF))
    return bytes(out)


def build(compiler, source, work, name):
    path = os.path.join(work, name + ".cee")
    with open(path, "w", encoding="utf-8") as f:
        f.write(source)
    exe = os.path.join(work, name + (".exe" if os.name == "nt" else ""))
    r = subprocess.run([compiler, "build", path, "-o", exe], capture_output=True, text=True, cwd=work)
    if r.returncode != 0:
        sys.exit(f"window_check: building {name} failed:\n{r.stderr}")
    return path, exe


def run(command, env, cwd):
    full = dict(os.environ)
    full.pop("CEESAFE_WINDOW_SCRIPT", None)
    full.pop("CEESAFE_WINDOW_DUMP", None)
    full.update(env)
    return subprocess.run(command, capture_output=True, text=True, env=full, cwd=cwd, timeout=120)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--display", action="store_true", help="also open a real window (needs a window server)")
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    with tempfile.TemporaryDirectory() as work:
        _, events = build(compiler, EVENTS, work, "events")
        script = os.path.join(work, "script.txt")
        with open(script, "w", encoding="utf-8") as f:
            f.write(SCRIPT)
        dump = os.path.join(work, "frame.bin")
        r = run([events], {"CEESAFE_WINDOW_SCRIPT": script, "CEESAFE_WINDOW_DUMP": dump}, work)
        want, last = expected(SCRIPT, 40, 30)
        got = r.stdout.splitlines()
        if r.returncode != 0 or got != want:
            problems.append("scripted events: expected\n  " + "\n  ".join(want) + "\ngot (status "
                            f"{r.returncode})\n  " + "\n  ".join(got) + "\n" + r.stderr)
        elif open(dump, "rb").read() != frame_bytes(*last):
            problems.append(f"scripted frame: the dump is not frame {last[1]} at {last[0]}")

        source, errors = build(compiler, ERRORS, work, "errors")
        bad = os.path.join(work, "bad.txt")
        with open(bad, "w", encoding="utf-8") as f:
            f.write("0:jump:1,2")
        cases = [
            ("a malformed script", [errors], {"CEESAFE_WINDOW_SCRIPT": bad}, "4"),
            ("a missing script", [errors], {"CEESAFE_WINDOW_SCRIPT": os.path.join(work, "none.txt")}, "4"),
            ("a zero size", [errors, "zero"], {"CEESAFE_WINDOW_SCRIPT": script}, "1"),
            ("another thread", [errors, "thread"], {"CEESAFE_WINDOW_SCRIPT": script}, "3"),
            ("a scripted open", [errors], {"CEESAFE_WINDOW_SCRIPT": script}, "0"),
            ("the interpreter", [compiler, "run", "--interp", source], {"CEESAFE_WINDOW_SCRIPT": script}, "1"),
        ]
        for what, command, env, answer in cases:
            r = run(command, env, work)
            if r.returncode != 0 or r.stdout.strip() != answer:
                problems.append(f"{what}: expected {answer}, got {r.stdout.strip()!r} (status {r.returncode}) "
                                f"{r.stderr.strip()}")

        surface_source, surface = build(compiler, SURFACE, work, "surface")
        surface_cases = [
            ("a scripted Frames window", [surface, "frames"], "0 0 0", 0, True),
            ("a scripted External window", [surface, "external"], "0 1 1", 0, False),
            ("openWith in the interpreter", [compiler, "run", "--interp", surface_source, "--", "external"],
             "open failed", 1, False),
        ]
        for what, command, answer, status, dumps in surface_cases:
            out = os.path.join(work, "surface.bin")
            if os.path.exists(out):
                os.remove(out)
            r = run(command, {"CEESAFE_WINDOW_SCRIPT": script, "CEESAFE_WINDOW_DUMP": out}, work)
            if r.returncode != status or r.stdout.strip() != answer:
                problems.append(f"{what}: expected {answer!r} (status {status}), got {r.stdout.strip()!r} "
                                f"(status {r.returncode}) {r.stderr.strip()}")
            elif os.path.exists(out) != dumps:
                problems.append(f"{what}: a frame was {'not ' if dumps else ''}dumped")

        _, changed = build(compiler, CHANGED, work, "changed")
        empty = os.path.join(work, "empty.txt")
        with open(empty, "w", encoding="utf-8") as f:
            f.write("")
        for steps in range(1, 9):
            out = os.path.join(work, f"changed{steps}.bin")
            r = run([changed, str(steps)], {"CEESAFE_WINDOW_SCRIPT": empty, "CEESAFE_WINDOW_DUMP": out}, work)
            if r.returncode != 0 or not os.path.exists(out) or open(out, "rb").read() != changed_expected(steps):
                problems.append(f"presentChanged, {steps} steps: the frame shown is not the expected one "
                                f"(status {r.returncode})")
        checks = 1 + len(cases) + len(surface_cases) + 8
        if args.display:
            r = run([surface, "external"], {}, work)
            checks += 1
            kind = "1" if sys.platform == "darwin" else "2" if os.name == "nt" else "3"
            if r.returncode != 0 or r.stdout.strip() != f"{kind} 1 1":
                problems.append(f"a real External window: expected '{kind} 1 1', got {r.stdout.strip()!r} "
                                f"(status {r.returncode}) {r.stderr.strip()}")
            _, display = build(compiler, DISPLAY, work, "display")
            r = run([display], {}, work)
            checks += 1
            if r.returncode != 0 or r.stdout.strip() != "320 200 scaled":
                problems.append(f"a real window: expected '320 200 scaled', got {r.stdout.strip()!r} "
                                f"(status {r.returncode}) {r.stderr.strip()}")
    for p in problems:
        print("FAIL", p)
    print(f"window_check: {checks} checks: {len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
