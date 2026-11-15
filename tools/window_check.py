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
4. text input and the clipboard (ADR 0141): a program that turns text input on and off and reads and writes the
   clipboard, frame by frame, must print the trace this script computes from the script's `compose`, `type` and
   `clipboard` items (a composition only while text input is on, an event only when it changes, `composition()` the
   latest one when the event is handed out, an end when text input goes off, the clipboard as the script and the
   program last set it); a caret inside a character is a malformed script;
5. waiting and waking (ADR 0142): a scripted program's waitEvent returns at once while the script holds events or one
   is queued, waits out its timeout once the script is used up, returns at once after a wake (two wakes are one), and
   returns when another thread wakes it, or a task run on another thread after it sent its result, during a wait of
   20 s; waitFrame takes a wake too, and a dropped window's waker wakes no later window; the interpreter answers
   Unsupported;
6. waiting for any window (ADR 0145): with two scripted windows, Window.waitAny returns at once while either has an
   event queued (the second's, once the first is drained, and a window opened later), waits out its timeout once
   both are drained, ends at a thread's wake of either window and at a task's, takes every window's wake, and finds a
   wake that the other window's waitEvent left; the interpreter answers Unsupported to the open;
7. with --display (a machine with a window server, not CI), a real window opens, reports its size and a scale of at
   least 1, takes frames and closes; an External one gives this system's handles, not zero; a real window's
   clipboard gives back the text put on it (this replaces the clipboard's contents); a task on another thread
   wakes a real window's waitEvent; with two real windows, a task's wake of the second ends a waitAny, and a click
   posted to the second through the system lands in its queue while only the first window's waitEvent and
   nextEvent run (ADR 0145 §2.2; not on X11, where windows do not share a connection), and ends a waitAny;
8. menus and file dialogs (ADR 0146): a scripted window accepts a menu, hands out the script's `menu` choices of
   enabled items only, answers each FileDialog function from the script's `dialog` items (and cancels without one), and
   a choice alone ends a waitEvent and a waitAny of 20 s; an item before the first menu is Unsupported, a menu id over
   32 bits a malformed script, and the interpreter answers Unsupported. With --display: on macOS, System Events reads
   the real menu bar (the application menu, the program's menus, an item's shortcut, enabled and check mark), clicks an
   item, types an enabled item's shortcut (a choice and no Key) and a disabled one's (a Key), and cancels one open panel
   and answers another by typing a path; on Windows, a WM_COMMAND posted for an item arrives as its id; on Linux,
   setMenu is Unsupported and the dialogs run a stand-in zenity put first on PATH (its arguments checked), and with no
   zenity or kdialog on PATH they are Unsupported.

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
        WindowEvent.Composition => { Console.writeLine("composition"); }
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

# Text input and the clipboard (ADR 0141): actions at the start of some frames, then the frame's events, then the
# clipboard. A real window ("real") puts a text on the clipboard and reads it back.
TEXT = r"""
fn clipboard(window: borrow mut Window) {
    var text = String.create();
    if (window.clipboardText(mut text)) {
        Console.write("clipboard [");
        Console.write(text.view());
        Console.writeLine("]");
    } else {
        Console.writeLine("clipboard none");
    }
}

fn main() -> i32 {
    var window = match (Window.open("window_check text", 40, 30)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    let real = match (Process.argument(1)) {
        Some(m) => m == "real",
        null => false,
    };
    if (real) {
        window.setTextInput(true);
        window.setTextInputArea(WindowBox { x: 10, y: 10, width: 2, height: 16 });
        match (window.setClipboardText("CeeSafe ✓ clipboard")) {
            Ok() => {}
            Err(_) => { Console.writeLine("set failed"); return 1; }
        }
        clipboard(mut window);
        window.setTextInput(false);
        return 0;
    }
    let pixels = [0xff000000u32; 4];
    for (frame in 0u32..12) {
        Console.write("frame ");
        Console.writeLine(frame);
        if (frame == 1 || frame == 7) {
            window.setTextInput(true);
            window.setTextInputArea(WindowBox { x: 3, y: 4, width: 1, height: 12 });
        }
        if (frame == 5) {
            window.setTextInput(false);
        }
        if (frame == 6) {
            match (window.setClipboardText("copied ✓")) {
                Ok() => {}
                Err(_) => { Console.writeLine("set failed"); }
            }
        }
        loop {
            match (window.nextEvent()) {
                Some(WindowEvent.Composition) => {
                    Console.write("composition [");
                    Console.write(window.composition());
                    Console.write("] ");
                    Console.writeLine(window.compositionCaret());
                }
                Some(WindowEvent.Text(c)) => { Console.write("text "); Console.writeLine(c); }
                Some(_) => { Console.writeLine("other"); }
                null => { break; }
            }
        }
        clipboard(mut window);
        window.present(pixels[..], 2, 2)!;
    }
    return 0;
}
"""

TEXT_SCRIPT = ("0:compose:1:x;0:clipboard;1:compose:3:か;1:compose:3:かな;2:compose:3:かな;2:compose:0:かな;"
               "3:type:仮名;3:compose:0:;4:compose:1:a;4:clipboard:from script ✓;5:compose:1:b;7:compose:2:é;"
               "8:clipboard:;9:clipboard;10:compose:0:")


# Waiting and waking (ADR 0142): each check prints its name and "ok" or "FAILED"; the times are generous bounds, since
# the point is that a wait returns at once, waits out its timeout, or ends at a wake long before its 20 s.
WAKE = r"""
async fn later(ms: u64) -> Result<void, AsyncError> {
    await Timer.sleep(ms)?;
    return Ok();
}

async fn deliver(out: Channel<u32>, waker: WindowWaker) -> Result<void, AsyncError> {
    await Timer.sleep(200)?;
    await out.sendAsync(7)?;
    waker.wake();
    return Ok();
}

fn timed(window: borrow mut Window, ms: u32) -> u64 {
    let start = Timer.now();
    window.waitEvent(ms);
    return Timer.now() - start;
}

fn drain(window: borrow mut Window, show: bool) {
    loop {
        match (window.nextEvent()) {
            Some(WindowEvent.Key(_, down, _, _)) => {
                if (show) {
                    Console.write("key ");
                    Console.writeLine(if (down) 1u32 else 0);
                }
            }
            Some(_) => { if (show) { Console.writeLine("other"); } }
            null => { break; }
        }
    }
}

fn verdict(what: StringView, ok: bool) {
    Console.write(what);
    Console.writeLine(if (ok) " ok" else " FAILED");
}

fn received(results: borrow Channel<u32>) -> u32 => match (results.tryRecv()) {
    Some(v) => v,
    null => 0,
};

fn closedWaker() -> WindowWaker {
    let window = Window.open("window_check closed", 10, 10)!;
    return window.waker();
}

fn main() -> i32 {
    var window = match (Window.open("window_check wake", 40, 30)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    let waker = window.waker();
    let real = match (Process.argument(1)) {
        Some(m) => m == "real",
        null => false,
    };
    let results = Channel<u32>.create(4)!;
    if (real) {
        // A real window may get events (focus, size) before the task's wake: wait until its result is there.
        let sender = results.clone();
        let start = Timer.now();
        let tasks = Thread.spawn(move () => Async.run(deliver(sender, waker)))!;
        var got: u32 = 0;
        loop {
            window.waitEvent(20000);
            drain(mut window, false);
            got = received(borrow results);
            if (got != 0 || Timer.now() - start >= 20000) {
                break;
            }
        }
        let elapsed = Timer.now() - start;
        let finished = tasks.join();
        verdict("real wake", got == 7 && elapsed >= 150 && elapsed < 5000);
        return 0;
    }
    let pixels = [0xff000000u32; 4];
    verdict("pending script", timed(mut window, 20000) < 5000);
    drain(mut window, true);
    window.present(pixels[..], 2, 2)!;
    verdict("queued event", timed(mut window, 20000) < 5000);
    drain(mut window, true);
    window.present(pixels[..], 2, 2)!;
    let waited = timed(mut window, 100);
    verdict("timeout", waited >= 90 && waited < 5000);
    verdict("zero", timed(mut window, 0) < 50);
    waker.wake();
    waker.wake();
    verdict("woken before", timed(mut window, 20000) < 5000);
    let again = timed(mut window, 100);
    verdict("wakes are one", again >= 90 && again < 5000);
    let thread = Thread.spawn(move () => {
        let slept = Async.run(later(200));
        waker.wake();
    })!;
    let woken = timed(mut window, 20000);
    thread.join();
    verdict("thread", woken >= 150 && woken < 10000);
    let sender = results.clone();
    let tasks = Thread.spawn(move () => Async.run(deliver(sender, waker)))!;
    let delivered = timed(mut window, 20000);
    let finished = tasks.join();
    verdict("task", received(borrow results) == 7 && delivered >= 150 && delivered < 10000);
    waker.wake();
    window.waitFrame();
    verdict("frame takes the wake", timed(mut window, 100) >= 90);
    let old = closedWaker();
    old.wake();
    verdict("closed", timed(mut window, 100) >= 90);
    return 0;
}
"""

WAKE_SCRIPT = "1:key:a"
WAKE_EXPECTED = ["pending script ok", "queued event ok", "key 1", "key 0", "timeout ok", "zero ok", "woken before ok",
                 "wakes are one ok", "thread ok", "task ok", "frame takes the wake ok", "closed ok"]


# Waiting for any window (ADR 0145): two scripted windows (each reads ANY_SCRIPT, so each has a key's down and up
# queued at open), or with "real" two real windows whose second a task wakes. The times are generous bounds, as
# WAKE's.
ANY = r"""
async fn later(ms: u64) -> Result<void, AsyncError> {
    await Timer.sleep(ms)?;
    return Ok();
}

async fn deliver(out: Channel<u32>, waker: WindowWaker) -> Result<void, AsyncError> {
    await Timer.sleep(200)?;
    await out.sendAsync(7)?;
    waker.wake();
    return Ok();
}

fn timedAny(ms: u32) -> u64 {
    let start = Timer.now();
    Window.waitAny(ms);
    return Timer.now() - start;
}

fn timed(window: borrow mut Window, ms: u32) -> u64 {
    let start = Timer.now();
    window.waitEvent(ms);
    return Timer.now() - start;
}

fn drain(window: borrow mut Window) -> u32 {
    var count: u32 = 0;
    loop {
        match (window.nextEvent()) {
            Some(_) => { count += 1; }
            null => { break; }
        }
    }
    return count;
}

fn verdict(what: StringView, ok: bool) {
    Console.write(what);
    Console.writeLine(if (ok) " ok" else " FAILED");
}

fn received(results: borrow Channel<u32>) -> u32 => match (results.tryRecv()) {
    Some(v) => v,
    null => 0,
};

fn closedWaker() -> WindowWaker {
    let window = Window.open("window_check any closed", 10, 10)!;
    return window.waker();
}

fn main() -> i32 {
    var a = match (Window.open("window_check any a", 40, 30)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    var b = Window.open("window_check any b", 40, 30)!;
    let first = a.waker();
    let second = b.waker();
    let real = match (Process.argument(1)) {
        Some(m) => m == "real",
        null => false,
    };
    let results = Channel<u32>.create(4)!;
    if (real) {
        // Real windows get events of their own (focus, size) before the task's wake: wait until its result is there.
        let sender = results.clone();
        let start = Timer.now();
        let tasks = Thread.spawn(move () => Async.run(deliver(sender, second)))!;
        var got: u32 = 0;
        loop {
            Window.waitAny(20000);
            let _ = drain(mut a);
            let _ = drain(mut b);
            got = received(borrow results);
            if (got != 0 || Timer.now() - start >= 20000) {
                break;
            }
        }
        let elapsed = Timer.now() - start;
        let finished = tasks.join();
        verdict("real wake of the second", got == 7 && elapsed >= 150 && elapsed < 5000);
        return 0;
    }
    verdict("both queued", timedAny(20000) < 5000);
    let ofFirst = drain(mut a);
    let quick = timedAny(20000);
    let ofSecond = drain(mut b);
    verdict("the second's event", quick < 5000 && ofFirst == 2 && ofSecond == 2);
    let waited = timedAny(100);
    verdict("timeout", waited >= 90 && waited < 5000);
    verdict("zero", timedAny(0) < 50);
    var c = Window.open("window_check any c", 40, 30)!;
    let opened = timedAny(20000);
    verdict("a later window's event", opened < 5000 && drain(mut c) == 2);
    let one = Thread.spawn(move () => {
        let slept = Async.run(later(200));
        second.wake();
    })!;
    let byOne = timedAny(20000);
    one.join();
    verdict("a thread wakes the second", byOne >= 150 && byOne < 10000);
    let two = Thread.spawn(move () => {
        let slept = Async.run(later(200));
        first.wake();
    })!;
    let byTwo = timedAny(20000);
    two.join();
    verdict("a thread wakes the first", byTwo >= 150 && byTwo < 10000);
    let sender = results.clone();
    let tasks = Thread.spawn(move () => Async.run(deliver(sender, second)))!;
    let delivered = timedAny(20000);
    let finished = tasks.join();
    verdict("task", received(borrow results) == 7 && delivered >= 150 && delivered < 10000);
    first.wake();
    second.wake();
    verdict("woken before", timedAny(20000) < 5000);
    verdict("every wake taken", timedAny(100) >= 90);
    second.wake();
    verdict("waitEvent leaves another's wake", timed(mut a, 100) >= 90);
    verdict("for waitAny", timedAny(20000) < 5000);
    first.wake();
    verdict("waitEvent takes its own", timed(mut a, 20000) < 5000);
    verdict("then waitAny waits", timedAny(100) >= 90);
    let old = closedWaker();
    old.wake();
    verdict("closed", timedAny(100) >= 90);
    return 0;
}
"""

ANY_SCRIPT = "0:key:a"
ANY_EXPECTED = ["both queued ok", "the second's event ok", "timeout ok", "zero ok", "a later window's event ok",
                "a thread wakes the second ok", "a thread wakes the first ok", "task ok", "woken before ok",
                "every wake taken ok", "waitEvent leaves another's wake ok", "for waitAny ok",
                "waitEvent takes its own ok", "then waitAny waits ok", "closed ok"]

# The routing of events between two real windows (ADR 0145 §2.2), with --display: a package whose C file posts a
# click (down and up) to the second window through the system (macOS: an NSEvent for its window number, posted to
# the application; Windows: WM_LBUTTONDOWN and WM_LBUTTONUP posted to its HWND). Only the first window's waitEvent and
# nextEvent then take the system's events, and the click must be in the second window's queue; then a second click
# must end a waitAny. On X11 each window has its own connection, so one window's pump never takes another's events:
# the helper posts nothing there and the program says so.
ROUTE = r"""
extern "C" {
    @effects()
    fn window_check_post_click(kind: u32, first: u64, second: u64) -> i32;
}

fn buttons(window: borrow mut Window) -> u32 {
    var count: u32 = 0;
    loop {
        match (window.nextEvent()) {
            Some(WindowEvent.PointerButton(_, _, _, _)) => { count += 1; }
            Some(_) => {}
            null => { break; }
        }
    }
    return count;
}

fn verdict(what: StringView, ok: bool) {
    Console.write(what);
    Console.writeLine(if (ok) " ok" else " FAILED");
}

/// Posts a click to the window through the system; false where the helper cannot.
/// Safety: the helper only reads the handles and posts two events to that window.
fn click(window: borrow Window) -> bool {
    var kind: u32 = 0;
    var x: u64 = 0;
    var y: u64 = 0;
    match (window.nativeHandle()) {
        WindowHandle.AppKit(p, q) => { kind = 1; x = p; y = q; }
        WindowHandle.Win32(p, q) => { kind = 2; x = p; y = q; }
        WindowHandle.Xlib(p, q) => { kind = 3; x = p; y = q; }
        WindowHandle.Scripted => {}
    }
    return unsafe { window_check_post_click(kind, x, y) } != 0;
}

fn main() -> i32 {
    var a = match (Window.open("window_check route a", 200, 150)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    var b = Window.open("window_check route b", 200, 150)!;
    // The windows' own first events (focus, size, the pointer) come and go.
    for (round in 0u32..10) {
        Window.waitAny(50);
        let _ = buttons(mut a);
        let _ = buttons(mut b);
    }
    if (!click(borrow b)) {
        Console.writeLine("post unsupported");
        return 0;
    }
    var ofFirst: u32 = 0;
    for (round in 0u32..10) {
        a.waitEvent(50);
        ofFirst += buttons(mut a);
    }
    let ofSecond = buttons(mut b);
    verdict("routed", ofFirst == 0 && ofSecond == 2);
    if (!click(borrow b)) {
        return 1;
    }
    let start = Timer.now();
    var got: u32 = 0;
    loop {
        Window.waitAny(20000);
        let _ = buttons(mut a);
        got += buttons(mut b);
        if (got >= 2 || Timer.now() - start >= 5000) {
            break;
        }
    }
    verdict("waitAny for the second's click", got == 2 && Timer.now() - start < 5000);
    return 0;
}
"""

# Menus and file dialogs (ADR 0146). Scripted: the window's menu set, MENU_SCRIPT's frame 0 choices handed out but for
# an unknown and a disabled item, each dialog function answered by the script's `dialog` items in order (a path, a
# cancel; several paths, the first alone for a one-path dialog), a dialog with no answer left cancelled, then frame 1's
# choice, the script's last item, ending a waitEvent of 20 s, and a second window's ending a waitAny; an item before the
# first menu is Unsupported and keeps the menu, and an empty menu is accepted. With "real" (--display, macOS), the
# menu is a real one that tools/window_check.py inspects and drives through System Events, reading each line the
# program prints.
MENU = r"""
fn say(text: StringView) {
    Console.writeLine(text);
    Console.flush();
}

fn sayNumber(text: StringView, n: u32) {
    Console.write(text);
    Console.writeLine(n);
    Console.flush();
}

fn verdict(what: StringView, ok: bool) {
    Console.write(what);
    Console.writeLine(if (ok) " ok" else " FAILED");
}

fn command(c: u32) -> MenuShortcut? => MenuShortcut {
    key: WindowKey.Character(c),
    modifiers: WindowModifiers { shift: false, control: false, alt: false, command: true },
};

fn menus() -> Menu {
    var menu = Menu.create();
    menu.addMenu("File")!;
    menu.addItem(1, "Open…", command(0x6F))!;
    menu.addItem(2, "Save", command(0x73))!;
    menu.addSeparator()!;
    menu.addItem(3, "Close", null)!;
    menu.addMenu("Edit")!;
    menu.addItem(4, "Undo", MenuShortcut {
        key: WindowKey.Function(5),
        modifiers: WindowModifiers { shift: true, control: false, alt: false, command: false },
    })!;
    menu.addItem(5, "Wrap", null)!;
    menu.setEnabled(3, false);
    menu.setChecked(5, true);
    return menu;
}

fn choices(window: borrow mut Window) {
    var line = String.create();
    line.append("choices")!;
    loop {
        match (window.nextMenuChoice()) {
            Some(id) => {
                line.append(" ")!;
                line.appendNumber(id)!;
            }
            null => { break; }
        }
    }
    Console.writeLine(line.view());
}

fn one(answer: Result<String?, WindowError>) {
    match (answer) {
        Ok(Some(path)) => {
            Console.write("path ");
            Console.writeLine(path.view());
        }
        Ok(null) => { Console.writeLine("cancelled"); }
        Err(e) => {
            Console.write("error ");
            Console.writeLine(codeOf(e));
        }
    }
    Console.flush();
}

fn several(answer: Result<Vector<String>, WindowError>) {
    match (answer) {
        Ok(paths) => {
            Console.write("paths");
            Console.writeLine(paths.length);
            for (k in 0..paths.length) {
                Console.write("  ");
                Console.writeLine(paths[k].view());
            }
        }
        Err(e) => {
            Console.write("error ");
            Console.writeLine(codeOf(e));
        }
    }
}

fn drain(window: borrow mut Window) -> u32 {
    var count: u32 = 0;
    loop {
        match (window.nextEvent()) {
            Some(_) => { count += 1; }
            null => { break; }
        }
    }
    return count;
}

fn timed(window: borrow mut Window, ms: u32) -> u64 {
    let start = Timer.now();
    window.waitEvent(ms);
    return Timer.now() - start;
}

fn timedAny(ms: u32) -> u64 {
    let start = Timer.now();
    Window.waitAny(ms);
    return Timer.now() - start;
}

fn codeOf(error: WindowError) -> u32 => match (error) {
    WindowError.Unsupported => 1,
    WindowError.NoDisplay => 2,
    WindowError.NotMainThread => 3,
    WindowError.Resources => 4,
};

fn errorCode(result: Result<void, WindowError>) -> u32 => match (result) {
    Ok() => 0,
    Err(e) => codeOf(e),
};

/// X11 (--display on Linux, with the checker's stand-in `zenity` first on PATH, or none): no menu bar, and the
/// dialogs through the program found on PATH.
fn x11(window: borrow mut Window) -> i32 {
    let menu = menus();
    sayNumber("setMenu ", errorCode(window.setMenu(borrow menu)));
    one(FileDialog.openFile(mut window, "Open", "png, jpg"));
    one(FileDialog.openFile(mut window, "cancel", ""));
    several(FileDialog.openFiles(mut window, "Open", "png"));
    one(FileDialog.saveFile(mut window, "Save", "name.txt", "txt"));
    one(FileDialog.chooseFolder(mut window, "Folder"));
    return 0;
}

/// The real menu (--display): choices and keys until the checker's clicks and keystrokes have come, then two dialogs.
fn real(window: borrow mut Window) -> i32 {
    var menu = Menu.create();
    menu.addMenu("File")!;
    menu.addItem(7, "Choose Seven", command(0x6F))!;
    menu.addItem(8, "Disabled Eight", command(0x64))!;
    menu.addSeparator()!;
    menu.addItem(9, "Checked Nine", null)!;
    menu.addMenu("Edit")!;
    menu.addItem(10, "Ten", null)!;
    menu.setEnabled(8, false);
    menu.setChecked(9, true);
    match (window.setMenu(borrow menu)) {
        Ok() => { say("menu set"); }
        Err(_) => { say("setMenu failed"); return 1; }
    }
    let start = Timer.now();
    var sevens: u32 = 0;
    var keysD: u32 = 0;
    while (sevens < 2 || keysD < 1) {
        if (Timer.now() - start > 60000) {
            say("timeout");
            return 1;
        }
        window.waitEvent(500);
        loop {
            match (window.nextEvent()) {
                Some(WindowEvent.Key(WindowKey.Character(c), true, _, _)) => {
                    if (c == 0x6F) {
                        say("key o");
                    }
                    if (c == 0x64) {
                        keysD += 1;
                        say("key d");
                    }
                }
                Some(_) => {}
                null => { break; }
            }
        }
        loop {
            match (window.nextMenuChoice()) {
                Some(id) => {
                    sayNumber("choice ", id);
                    if (id == 7) {
                        sevens += 1;
                    }
                }
                null => { break; }
            }
        }
    }
    say("dialog");
    one(FileDialog.openFile(mut window, "window_check cancel", "txt"));
    say("dialog");
    one(FileDialog.openFile(mut window, "window_check answer", "txt"));
    return 0;
}

fn main() -> i32 {
    var a = match (Window.open("window_check menu", 40, 30)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    let isReal = match (Process.argument(1)) {
        Some(m) => m == "real",
        null => false,
    };
    if (isReal) {
        return real(mut a);
    }
    let isX11 = match (Process.argument(1)) {
        Some(m) => m == "x11",
        null => false,
    };
    if (isX11) {
        return x11(mut a);
    }
    let menu = menus();
    verdict("set", errorCode(a.setMenu(borrow menu)) == 0);
    choices(mut a);
    one(FileDialog.openFile(mut a, "Open", "png, .jpg"));
    one(FileDialog.openFile(mut a, "Open", ""));
    several(FileDialog.openFiles(mut a, "Open", "png"));
    several(FileDialog.openFiles(mut a, "Open", "png"));
    one(FileDialog.saveFile(mut a, "Save", "untitled.txt", "txt"));
    one(FileDialog.saveFile(mut a, "Save", "untitled.txt", "txt"));
    one(FileDialog.chooseFolder(mut a, "Folder"));
    one(FileDialog.chooseFolder(mut a, "Folder"));
    one(FileDialog.openFile(mut a, "Open", ""));
    let pixels = [0u32; 1200];
    a.present(pixels[..], 40, 30)!;
    let _ = drain(mut a);
    verdict("a choice ends waitEvent", timed(mut a, 20000) < 5000);
    choices(mut a);
    verdict("then waitEvent waits", timed(mut a, 100) >= 90);
    var b = Window.open("window_check menu b", 40, 30)!;
    verdict("the second's menu set", errorCode(b.setMenu(borrow menu)) == 0);
    let _ = drain(mut b);
    choices(mut b);
    b.present(pixels[..], 40, 30)!;
    let _ = drain(mut b);
    verdict("a choice ends waitAny", timedAny(20000) < 5000);
    choices(mut a);
    choices(mut b);
    verdict("then waitAny waits", timedAny(100) >= 90);
    var bad = Menu.create();
    bad.addItem(1, "No menu", null)!;
    sayNumber("an item before a menu ", errorCode(b.setMenu(borrow bad)));
    let empty = Menu.create();
    verdict("an empty menu", errorCode(b.setMenu(borrow empty)) == 0);
    return 0;
}
"""

MENU_SCRIPT = ("0:menu:1;0:menu:99;0:menu:3;0:menu:2;0:dialog:/tmp/ünï.png;0:dialog;0:dialog:/x/one|/x/two;0:dialog;"
               "0:dialog:/y/save.txt;0:dialog;0:dialog:/z/folder|/z/other;0:dialog;1:menu:4")
MENU_EXPECTED = ["set ok", "choices 1 2", "path /tmp/ünï.png", "cancelled", "paths2", "  /x/one", "  /x/two", "paths0",
                 "path /y/save.txt", "cancelled", "path /z/folder", "cancelled", "cancelled",
                 "a choice ends waitEvent ok", "choices 4", "then waitEvent waits ok", "the second's menu set ok",
                 "choices 1 2", "a choice ends waitAny ok", "choices", "choices 4", "then waitAny waits ok",
                 "an item before a menu 1", "an empty menu ok"]

# Windows' menu commands (ADR 0146), with --display there: a package whose C file posts WM_COMMAND for the menu's
# first and third items (commands 1 and 3) to the window, which must arrive as the items' ids, 7 and 9.
MENU_POST = r"""
extern "C" {
    @effects()
    fn window_check_post_command(kind: u32, first: u64, command: u32) -> i32;
}

/// Posts WM_COMMAND to the window; false where the helper cannot.
/// Safety: the helper only reads the handle and posts one message to that window.
fn post(window: borrow Window, command: u32) -> bool => match (window.nativeHandle()) {
    WindowHandle.Win32(hwnd, _) => unsafe { window_check_post_command(2, hwnd, command) } != 0,
    _ => false,
};

fn main() -> i32 {
    var w = match (Window.open("window_check menu post", 200, 150)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    var menu = Menu.create();
    menu.addMenu("File")!;
    menu.addItem(7, "Seven", null)!;
    menu.addItem(8, "Eight", null)!;
    menu.addSeparator()!;
    menu.addItem(9, "Nine", null)!;
    w.setMenu(borrow menu)!;
    if (!post(borrow w, 1) || !post(borrow w, 3)) {
        Console.writeLine("post unsupported");
        return 0;
    }
    var line = String.create();
    line.append("choices")!;
    let start = Timer.now();
    while (Timer.now() - start < 3000) {
        w.waitEvent(200);
        loop {
            match (w.nextEvent()) {
                Some(_) => {}
                null => { break; }
            }
        }
        loop {
            match (w.nextMenuChoice()) {
                Some(id) => {
                    line.append(" ")!;
                    line.appendNumber(id)!;
                }
                null => { break; }
            }
        }
    }
    Console.writeLine(line.view());
    return 0;
}
"""

MENU_POST_C = r"""/* window_check's helper (ADR 0146): posts a menu's WM_COMMAND to a window, on Windows. */
#include <stdint.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#endif
#endif

int32_t window_check_post_command(uint32_t kind, uint64_t first, uint32_t command);

int32_t window_check_post_command(uint32_t kind, uint64_t first, uint32_t command) {
#if defined(_WIN32)
    return kind == 2 && PostMessageW((HWND)(uintptr_t)first, WM_COMMAND, MAKEWPARAM(command, 0), 0);
#else
    (void)kind;
    (void)first;
    (void)command;
    return 0;
#endif
}
"""

# The stand-in for zenity (ADR 0146, --display on Linux): it records its arguments, and answers as a user would.
FAKE_ZENITY = r"""#!/bin/sh
for a in "$@"; do printf '[%s]\n' "$a" >> "$WINDOW_CHECK_ARGS"; done
echo --- >> "$WINDOW_CHECK_ARGS"
case "$*" in
  *--title=cancel*) exit 1 ;;
  *--multiple*) printf '/fake/one.png\n/fake/two.png\n' ;;
  *--directory*) echo /fake/folder ;;
  *--save*) echo /fake/save.txt ;;
  *) echo /fake/open.png ;;
esac
"""
X11_EXPECTED = ["setMenu 1", "path /fake/open.png", "cancelled", "paths2", "  /fake/one.png", "  /fake/two.png",
                "path /fake/save.txt", "path /fake/folder"]
X11_ARGUMENTS = ["[--file-selection]", "[--title=Open]", "[--file-filter=*.png *.jpg]", "[--multiple]",
                 "[--separator=\n]", "[--save]", "[--filename=name.txt]", "[--file-filter=*.txt]", "[--directory]"]


def mac_menu(exe, work):
    """ADR 0146 on macOS with a real menu bar: System Events reads the menus, chooses an item, types a shortcut of an
    enabled item (a choice, no Key) and of a disabled one (a Key), cancels one open panel and answers another. A list
    of problems."""
    import queue
    import threading
    import time
    name = os.path.basename(exe)
    env = dict(os.environ)
    env.pop("CEESAFE_WINDOW_SCRIPT", None)
    env.pop("CEESAFE_WINDOW_DUMP", None)
    p = subprocess.Popen([exe, "real"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, cwd=work)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line.rstrip("\n")) for line in p.stdout], daemon=True).start()
    seen = []

    def expect(want, seconds=20):
        end = time.time() + seconds
        while time.time() < end:
            try:
                line = lines.get(timeout=0.2)
            except queue.Empty:
                continue
            seen.append(line)
            if line == want:
                return
        raise RuntimeError(f"expected {want!r}, the program printed {seen!r}")

    def osa(*statements):
        command = ["osascript"]
        for s in statements:
            command += ["-e", s]
        r = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            raise RuntimeError(f"osascript {statements!r}: {r.stderr.strip()}")
        return r.stdout.strip()

    events = 'tell application "System Events" to '
    process = events + f'tell process "{name}" to '
    file_menu = 'menu "File" of menu bar item "File" of menu bar 1'
    problems = []
    try:
        expect("menu set")
        bar = osa(process + "get name of every menu bar item of menu bar 1")
        if bar != f"Apple, {name}, File, Edit":
            problems.append(f"a real menu bar: expected 'Apple, {name}, File, Edit', got {bar!r}")
        items = osa(process + f"get {{name, enabled}} of every menu item of {file_menu}")
        want = "Choose Seven, Disabled Eight, missing value, Checked Nine, true, false, false, true"
        if items != want:
            problems.append(f"a real menu's items: expected {want!r}, got {items!r}")
        shortcut = osa(process + f'get value of attribute "AXMenuItemCmdChar" of menu item "Choose Seven" of {file_menu}')
        mark = osa(process + f'get value of attribute "AXMenuItemMarkChar" of menu item "Checked Nine" of {file_menu}')
        if shortcut != "O" or mark != "✓":
            problems.append(f"a real menu's shortcut and check mark: expected 'O' and '✓', got {shortcut!r} {mark!r}")
        osa(process + f'click menu item "Choose Seven" of {file_menu}')
        expect("choice 7")
        osa(process + "set frontmost to true", "delay 0.5", events + 'keystroke "o" using command down')
        expect("choice 7")
        osa(events + 'keystroke "d" using command down')
        expect("key d")
        if "key o" in seen:
            problems.append("a real menu's shortcut also gave a Key event")
        expect("dialog")
        answer = os.path.realpath(os.path.join(work, "answer.txt"))
        with open(answer, "w", encoding="utf-8") as f:
            f.write("window_check\n")
        for title in ("window_check cancel", "window_check answer"):
            osa("repeat 40 times", 'tell application "System Events"',
                f'if exists window "{title}" of process "{name}" then exit repeat', "end tell", "delay 0.25",
                "end repeat", "delay 0.5")
            if title.endswith("cancel"):
                osa(events + "key code 53")
                expect("cancelled")
                expect("dialog")
            else:
                osa(events + 'keystroke "g" using {command down, shift down}', "delay 1.5",
                    events + f'keystroke "{answer}"', "delay 2", events + "key code 36", "delay 2",
                    events + "key code 36")
                expect("path " + answer)
        if p.wait(timeout=20) != 0:
            problems.append(f"a real menu: the program ended with status {p.returncode}")
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        problems.append(f"a real menu and dialogs: {e}")
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
    return problems


ROUTE_C = r"""/* window_check's helper (ADR 0145): posts a primary click at (10, 10) in a window, through the system. */
#include <stdint.h>
#if defined(__APPLE__)
#include <dlfcn.h>
#include <string.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#endif
#endif

int32_t window_check_post_click(uint32_t kind, uint64_t first, uint64_t second);

#if defined(__APPLE__)
typedef void *cee_id;
typedef struct {
    double x, y;
} cee_point;

static void *cee_symbol(void *library, const char *name) { return dlsym(library, name); }
#endif

int32_t window_check_post_click(uint32_t kind, uint64_t first, uint64_t second) {
    (void)second;
#if defined(__APPLE__)
    if (kind != 1) {
        return 0;
    }
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY);
    if (objc == NULL) {
        return 0;
    }
    void *p[5] = {cee_symbol(objc, "objc_getClass"), cee_symbol(objc, "sel_registerName"),
                  cee_symbol(objc, "objc_msgSend"), cee_symbol(objc, "objc_autoreleasePoolPush"),
                  cee_symbol(objc, "objc_autoreleasePoolPop")};
    cee_id (*get_class)(const char *);
    cee_id (*sel)(const char *);
    void (*send)(void);
    void *(*push)(void);
    void (*pop)(void *);
    memcpy(&get_class, &p[0], sizeof get_class);
    memcpy(&sel, &p[1], sizeof sel);
    memcpy(&send, &p[2], sizeof send);
    memcpy(&push, &p[3], sizeof push);
    memcpy(&pop, &p[4], sizeof pop);
    void *pool = push();
    cee_id window = (cee_id)(uintptr_t)first;
    long number = ((long (*)(cee_id, cee_id))send)(window, sel("windowNumber"));
    cee_id app = ((cee_id(*)(cee_id, cee_id))send)(get_class("NSApplication"), sel("sharedApplication"));
    cee_point at = {10.0, 10.0}; /* the window's coordinates: from its bottom left, inside the content */
    for (unsigned long type = 1; type <= 2; type++) { /* NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp */
        cee_id event = ((cee_id(*)(cee_id, cee_id, unsigned long, cee_point, unsigned long, double, long, cee_id, long,
                                   long, float))send)(
            get_class("NSEvent"),
            sel("mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:clickCount:"
                "pressure:"),
            type, at, 0ul, 0.0, number, NULL, 0l, 1l, 1.0f);
        if (event == NULL) {
            pop(pool);
            return 0;
        }
        ((void (*)(cee_id, cee_id, cee_id, signed char))send)(app, sel("postEvent:atStart:"), event, 0);
    }
    pop(pool);
    return 1;
#elif defined(_WIN32)
    if (kind != 2) {
        return 0;
    }
    HWND hwnd = (HWND)(uintptr_t)first;
    return PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10)) &&
           PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
#else
    (void)kind;
    (void)first;
    return 0;
#endif
}
"""


def text_expected(script):
    """The trace of TEXT run with `script`, by §26.13's grammar and ADR 0141's rules."""
    items = {}
    for item in (i for i in script.split(";") if i):
        frame, what, *rest = item.split(":", 2)
        items.setdefault(int(frame), []).append((what, rest[0] if rest else None))
    lines, on, live, shown, clip = [], None, ("", 0), ("", 0), None
    for frame in range(12):
        lines.append(f"frame {frame}")
        queue = []
        if frame in (1, 7):
            on = True
        if frame == 5:
            if live != ("", 0):
                live = ("", 0)
                queue.append("composition")
            on = False
        if frame == 6:
            clip = "copied ✓"
        for what, args in items.get(frame, []):
            if what == "compose":
                caret, text = args.split(":", 1)
                if on and (text, int(caret)) != live:
                    live = (text, int(caret))
                    queue.append("composition")
            elif what == "type":
                queue += [f"text {ord(c)}" for c in args]
            elif what == "clipboard":
                clip = args
        for e in queue:
            if e == "composition":
                shown = live
                lines.append(f"composition [{shown[0]}] {shown[1]}")
            else:
                lines.append(e)
        lines.append("clipboard none" if clip is None else f"clipboard [{clip}]")
    return lines


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


def build_package(compiler, source, c_source, work, name):
    """A package of one CeeSafe file and one C file (type-system.md §28.6), built to an executable."""
    root = os.path.join(work, name + "-package")
    os.makedirs(os.path.join(root, "src"), exist_ok=True)
    with open(os.path.join(root, "ceesafe.toml"), "w", encoding="utf-8") as f:
        f.write(f'[package]\nname = "{name}"\nc = ["src/helper.c"]\n')
    with open(os.path.join(root, "src", "main.cee"), "w", encoding="utf-8") as f:
        f.write(source)
    with open(os.path.join(root, "src", "helper.c"), "w", encoding="utf-8") as f:
        f.write(c_source)
    exe = os.path.join(work, name + (".exe" if os.name == "nt" else ""))
    r = subprocess.run([compiler, "build", "-o", exe], capture_output=True, text=True, cwd=root)
    if r.returncode != 0:
        sys.exit(f"window_check: building {name} failed:\n{r.stderr}")
    return exe


def run(command, env, cwd):
    full = dict(os.environ)
    full.pop("CEESAFE_WINDOW_SCRIPT", None)
    full.pop("CEESAFE_WINDOW_DUMP", None)
    full.update(env)
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", env=full,
                          cwd=cwd, timeout=120)


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

        text_source, text = build(compiler, TEXT, work, "text")
        text_script = os.path.join(work, "text.txt")
        with open(text_script, "w", encoding="utf-8") as f:
            f.write(TEXT_SCRIPT)
        r = run([text], {"CEESAFE_WINDOW_SCRIPT": text_script}, work)
        want, got = text_expected(TEXT_SCRIPT), r.stdout.splitlines()
        if r.returncode != 0 or got != want:
            problems.append("text input and the clipboard: expected\n  " + "\n  ".join(want) + "\ngot (status "
                            f"{r.returncode})\n  " + "\n  ".join(got) + "\n" + r.stderr)
        malformed = {}
        for name, item in (("inside", "0:compose:1:é"), ("nocaret", "0:compose:かな")):
            malformed[name] = os.path.join(work, name + ".txt")
            with open(malformed[name], "w", encoding="utf-8") as f:
                f.write(item)
        text_cases = [
            ("a caret inside a character", [errors], {"CEESAFE_WINDOW_SCRIPT": malformed["inside"]}, "4"),
            ("a compose without a caret", [errors], {"CEESAFE_WINDOW_SCRIPT": malformed["nocaret"]}, "4"),
            ("text input in the interpreter", [compiler, "run", "--interp", text_source],
             {"CEESAFE_WINDOW_SCRIPT": text_script}, "open failed"),
        ]
        for what, command, env, answer in text_cases:
            r = run(command, env, work)
            status = 1 if answer == "open failed" else 0
            if r.returncode != status or r.stdout.strip() != answer:
                problems.append(f"{what}: expected {answer}, got {r.stdout.strip()!r} (status {r.returncode}) "
                                f"{r.stderr.strip()}")

        wake_source, wake = build(compiler, WAKE, work, "wake")
        wake_script = os.path.join(work, "wake.txt")
        with open(wake_script, "w", encoding="utf-8") as f:
            f.write(WAKE_SCRIPT)
        wake_cases = [
            ("waiting and waking", [wake], WAKE_EXPECTED, 0),
            ("waking in the interpreter", [compiler, "run", "--interp", wake_source], ["open failed"], 1),
        ]
        for what, command, want, status in wake_cases:
            r = run(command, {"CEESAFE_WINDOW_SCRIPT": wake_script}, work)
            got = r.stdout.splitlines()
            if r.returncode != status or got != want:
                problems.append(f"{what}: expected\n  " + "\n  ".join(want) + f"\ngot (status {r.returncode})\n  " +
                                "\n  ".join(got) + "\n" + r.stderr)

        any_source, any_program = build(compiler, ANY, work, "any")
        any_script = os.path.join(work, "any.txt")
        with open(any_script, "w", encoding="utf-8") as f:
            f.write(ANY_SCRIPT)
        any_cases = [
            ("waiting for any window", [any_program], ANY_EXPECTED, 0),
            ("waitAny in the interpreter", [compiler, "run", "--interp", any_source], ["open failed"], 1),
        ]
        for what, command, want, status in any_cases:
            r = run(command, {"CEESAFE_WINDOW_SCRIPT": any_script}, work)
            got = r.stdout.splitlines()
            if r.returncode != status or got != want:
                problems.append(f"{what}: expected\n  " + "\n  ".join(want) + f"\ngot (status {r.returncode})\n  " +
                                "\n  ".join(got) + "\n" + r.stderr)

        menu_source, menu = build(compiler, MENU, work, "window_check_menu")
        menu_script = os.path.join(work, "menu.txt")
        with open(menu_script, "w", encoding="utf-8") as f:
            f.write(MENU_SCRIPT)
        bad_menu = os.path.join(work, "badmenu.txt")
        with open(bad_menu, "w", encoding="utf-8") as f:
            f.write("0:menu:4294967296")
        menu_cases = [
            ("menus and file dialogs", [menu], menu_script, MENU_EXPECTED, 0),
            ("menus in the interpreter", [compiler, "run", "--interp", menu_source], menu_script, ["open failed"], 1),
            ("a menu id over 32 bits", [menu], bad_menu, ["open failed"], 1),
        ]
        for what, command, path, want, status in menu_cases:
            r = run(command, {"CEESAFE_WINDOW_SCRIPT": path}, work)
            got = r.stdout.splitlines()
            if r.returncode != status or got != want:
                problems.append(f"{what}: expected\n  " + "\n  ".join(want) + f"\ngot (status {r.returncode})\n  " +
                                "\n  ".join(got) + "\n" + r.stderr)

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
        checks = (1 + len(cases) + len(surface_cases) + 1 + len(text_cases) + len(wake_cases) + len(any_cases) +
                  len(menu_cases) + 8)
        if args.display:
            r = run([surface, "external"], {}, work)
            checks += 1
            kind = "1" if sys.platform == "darwin" else "2" if os.name == "nt" else "3"
            if r.returncode != 0 or r.stdout.strip() != f"{kind} 1 1":
                problems.append(f"a real External window: expected '{kind} 1 1', got {r.stdout.strip()!r} "
                                f"(status {r.returncode}) {r.stderr.strip()}")
            r = run([text, "real"], {}, work)
            checks += 1
            if r.returncode != 0 or r.stdout.strip() != "clipboard [CeeSafe ✓ clipboard]":
                problems.append(f"a real window's clipboard: expected 'clipboard [CeeSafe ✓ clipboard]', got "
                                f"{r.stdout.strip()!r} (status {r.returncode}) {r.stderr.strip()}")
            r = run([wake, "real"], {}, work)
            checks += 1
            if r.returncode != 0 or r.stdout.strip() != "real wake ok":
                problems.append(f"a real window's wake: expected 'real wake ok', got {r.stdout.strip()!r} "
                                f"(status {r.returncode}) {r.stderr.strip()}")
            r = run([any_program, "real"], {}, work)
            checks += 1
            if r.returncode != 0 or r.stdout.strip() != "real wake of the second ok":
                problems.append(f"two real windows' waitAny: expected 'real wake of the second ok', got "
                                f"{r.stdout.strip()!r} (status {r.returncode}) {r.stderr.strip()}")
            route = build_package(compiler, ROUTE, ROUTE_C, work, "route")
            r = run([route], {}, work)
            checks += 1
            want = (["post unsupported"] if sys.platform.startswith("linux") else
                    ["routed ok", "waitAny for the second's click ok"])
            if r.returncode != 0 or r.stdout.splitlines() != want:
                problems.append(f"two real windows' events: expected {want!r}, got {r.stdout.splitlines()!r} "
                                f"(status {r.returncode}) {r.stderr.strip()}")
            checks += 1
            if sys.platform == "darwin":
                problems.extend(mac_menu(menu, work))
            elif os.name == "nt":
                post = build_package(compiler, MENU_POST, MENU_POST_C, work, "menupost")
                r = run([post], {}, work)
                if r.returncode != 0 or r.stdout.strip() != "choices 7 9":
                    problems.append(f"a real window's menu commands: expected 'choices 7 9', got "
                                    f"{r.stdout.strip()!r} (status {r.returncode}) {r.stderr.strip()}")
            else:
                fake = os.path.join(work, "fakebin")
                os.makedirs(fake, exist_ok=True)
                with open(os.path.join(fake, "zenity"), "w", encoding="utf-8") as f:
                    f.write(FAKE_ZENITY)
                os.chmod(os.path.join(fake, "zenity"), 0o755)
                arguments = os.path.join(work, "zenity-arguments.txt")
                r = run([menu, "x11"], {"PATH": fake + os.pathsep + os.environ.get("PATH", ""),
                                        "WINDOW_CHECK_ARGS": arguments, "XDG_CURRENT_DESKTOP": "GNOME"}, work)
                recorded = open(arguments, encoding="utf-8").read() if os.path.exists(arguments) else ""
                missing = [a for a in X11_ARGUMENTS if a not in recorded]
                if r.returncode != 0 or r.stdout.splitlines() != X11_EXPECTED or missing:
                    problems.append("X11's menu and dialogs: expected\n  " + "\n  ".join(X11_EXPECTED) +
                                    f"\ngot (status {r.returncode})\n  " + "\n  ".join(r.stdout.splitlines()) +
                                    f"\nzenity's arguments missing {missing!r}\n" + r.stderr)
                empty_path = os.path.join(work, "emptybin")
                os.makedirs(empty_path, exist_ok=True)
                r = run([menu, "x11"], {"PATH": empty_path}, work)
                checks += 1
                if r.returncode != 0 or r.stdout.splitlines()[1:2] != ["error 1"]:
                    problems.append(f"X11's dialogs with no zenity or kdialog: expected 'error 1', got "
                                    f"{r.stdout.splitlines()!r} (status {r.returncode}) {r.stderr.strip()}")
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
