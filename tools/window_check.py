#!/usr/bin/env python3
"""Checks of lib/std's Window (type-system.md §26.13, ADR 0069) that the corpus cannot make, since a corpus program
that opened a window would open one on the developer's screen:

1. with CEESAFE_WINDOW_SCRIPT, a program that prints every event it gets, frame by frame, and presents a pattern of
   each frame's size and number: its output must equal the trace this script computes from the same script by the
   grammar of §26.13 (click's up on the next frame, key's down then up, one Text per character, a resize or focus
   only when it changes something, delivery at the frame after the n-th present), and CEESAFE_WINDOW_DUMP must hold
   the last frame, byte for byte;
2. the errors: a malformed script and a missing one are Resources, a zero size is Unsupported, an open on a thread
   other than the window thread is NotMainThread (on macOS the first thread; elsewhere the first that opened one,
   ADR 0201), and the interpreter answers Unsupported;
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
   zenity or kdialog on PATH they are Unsupported;
9. file drops (ADR 0150): a scripted window's drag (`dragover`, `dragleave`) shows in dragOver, its drops arrive with
   their position and paths (one that is not UTF-8 left out, a drop of only such paths skipped) and end the drag, and a
   drag change alone and a drop alone each end a waitEvent and a waitAny of 20 s; a dragover of no files or without a
   count, a drop without paths and a dragleave with arguments are malformed scripts, and the interpreter answers
   Unsupported. With --display, a person's drags made by a helper built with the C compiler (`cc`, `gcc` on Windows,
   or $CC): on macOS the system's mouse events from a small Objective-C program whose window starts a dragging
   session, on Windows the same from a C program whose window starts an OLE drag (DoDragDrop), moved by SendInput, on
   X11 an XDND source in C: a drag of text is refused, a drag of two files that enters and leaves shows in dragOver
   and then not, and a dropped one arrives with both paths at the point of release;
10. touches (ADR 0155): with touches off, a scripted window's first finger is the pointer and the others are ignored;
   turned on, the pointer's finger lifts it, and every finger gets the runtime's numbers from 1, never reused, its
   moves only for a change of place, a move before an end that moved, and an end: Ended, or Cancelled when the script
   cancels it, focus is lost, touches are turned off or the window closes; a full queue merges a finger's moves, then
   drops the oldest move, then the oldest whole finger; a touch ends a waitEvent and a waitAny of 20 s; a touch
   without its place, its number or a known phase is a malformed script, and the interpreter answers Unsupported.
   With --display, a real desktop window has no touches (setTouches is false).
11. the safe area (ADR 0156): a scripted window's safe area is zero until its script gives one; a new one shows in
   safeArea with a Resized at the content's size, the same one again gives nothing, and one given with a resize comes
   after it; a safe area of three or five numbers or with a negative one is a malformed script, and the interpreter
   answers Unsupported. With --display, a real desktop window's safe area is zero.
12. the soft keyboard (ADR 0157): a scripted window's keyboard inset is zero until its script gives one; a new one
   shows in keyboardInset with a Resized at the content's size, the same one again gives nothing, one taller than the
   content is the content's height, and setTextInputKind and setReturnKey are accepted; a keyboard of two numbers or a negative one is
   a malformed script. With --display, a real desktop window's keyboard inset is zero.
13. exporting a file (ADR 0158): on a scripted window FileDialog.exportFile copies the program's file to the path its
   script's `dialog` item answers and is true, leaves the file as it was, is false without an answer (a cancel), and
   is Resources when the file to export does not exist.
14. the app's life (ADR 0159): a scripted window's app events come from nextAppEvent in order, Background and
   Foreground alternating (one that says what the last said is not given), with memory warnings in either state;
   an app event ends a waitEvent of 20 s; an unknown app event is a malformed script. With --display, a real desktop
   window has none.
15. accessibility (ADR 0164): a scripted window's `access` items come from takeAccessAction in order when the list
   given to setAccessNodes has the element, and not when it has not; an id of 0 or an unknown action is a malformed
   script; an id given twice and a label outside the text are panics;
   a text field's selection is its node's caret and anchor; accessActive is false. With --display on
   macOS, AppKit reports each of the eleven roles back with its name, value, state and frame, an action performed
   through it arrives, an element is the same object from one list to the next, and accessActive is then true.

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
    } else if (mode == "thread-after") {
        // ADR 0201: once a thread has opened a window, another thread's open is refused on every system.
        let first = Window.open("window_check", 20, 20);
        let t = Thread.spawn(move () => openCode(20))!;
        Console.writeLine(t.join());
        let _ = first;
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
        WindowHandle.UIKit(p, q) => { kind = 4; x = p; y = q; }
        WindowHandle.Android(p, q) => { kind = 5; x = p; y = q; }
        WindowHandle.Wayland(p, q) => { kind = 6; x = p; y = q; }
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

# File drops (ADR 0150): the trace of the script's drags and drops, frame by frame; with "wait", a drag change and a
# drop each ending a wait alone; with "real", a real window (--display) that prints its drag's changes and its first
# drop, after "ready" (with the window's XID on X11).
DROP = r"""
fn say(text: StringView) {
    Console.writeLine(text);
    Console.flush();
}

fn verdict(what: StringView, ok: bool) {
    Console.write(what);
    Console.writeLine(if (ok) " ok" else " FAILED");
}

fn over(window: borrow Window) {
    match (window.dragOver()) {
        Some(d) => {
            Console.write("over ");
            Console.write(d.x);
            Console.write(" ");
            Console.write(d.y);
            Console.write(" ");
            Console.writeLine(d.count);
        }
        null => { Console.writeLine("over none"); }
    }
    Console.flush();
}

fn drops(window: borrow mut Window) -> u32 {
    var count: u32 = 0;
    loop {
        match (window.nextDrop()) {
            Some(d) => {
                count += 1;
                Console.write("drop ");
                Console.write(d.x);
                Console.write(" ");
                Console.writeLine(d.y);
                for (k in 0..d.paths.length) {
                    Console.write("  ");
                    Console.writeLine(d.paths[k].view());
                }
            }
            null => { break; }
        }
    }
    if (count == 0) {
        Console.writeLine("no drop");
    }
    Console.flush();
    return count;
}

fn drain(window: borrow mut Window) {
    loop {
        match (window.nextEvent()) {
            Some(_) => {}
            null => { break; }
        }
    }
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

/// The script's frames 0 to 5: after each present, the events, then the drag, the drops, and the drag again.
fn trace(window: borrow mut Window) -> i32 {
    over(window);
    let _ = drops(window);
    let pixels = [0u32; 1200];
    for (frame in 0..5) {
        window.present(pixels[..], 40, 30)!;
        drain(window);
        over(window);
        let _ = drops(window);
        over(window);
    }
    return 0;
}

/// The script's one item, at frame 1, ends a waitEvent of 20 s, and in a second window a waitAny of 20 s.
fn waits(a: borrow mut Window) -> i32 {
    let pixels = [0u32; 1200];
    a.present(pixels[..], 40, 30)!;
    verdict("ends waitEvent", timed(a, 20000) < 5000);
    over(a);
    let _ = drops(a);
    verdict("then waitEvent waits", timed(a, 100) >= 90);
    var b = Window.open("window_check drop b", 40, 30)!;
    drain(mut b);
    b.present(pixels[..], 40, 30)!;
    verdict("ends waitAny", timedAny(20000) < 5000);
    over(borrow b);
    let _ = drops(mut b);
    verdict("then waitAny waits", timedAny(100) >= 90);
    return 0;
}

/// A real window: each change of its drag, and its first drop, for at most 60 s.
fn real(w: borrow mut Window) -> i32 {
    let pixels = [0xFF808080u32; 60000];
    w.present(pixels[..], 300, 200)!;
    match (w.nativeHandle()) {
        WindowHandle.Xlib(_, xid) => {
            Console.write("ready ");
            Console.writeLine(xid);
            Console.flush();
        }
        _ => { say("ready"); }
    }
    let start = Timer.now();
    var last: u32 = 0;
    var lastX: f32 = -1.0;
    var lastY: f32 = -1.0;
    while (Timer.now() - start < 60000) {
        w.waitEvent(500);
        drain(w);
        match (w.dragOver()) {
            Some(d) => {
                if (d.count != last || d.x != lastX || d.y != lastY) {
                    over(w);
                }
                last = d.count;
                lastX = d.x;
                lastY = d.y;
            }
            null => {
                if (last != 0) {
                    say("over none");
                }
                last = 0;
            }
        }
        match (w.nextDrop()) {
            Some(d) => {
                Console.write("drop ");
                Console.write(d.x);
                Console.write(" ");
                Console.writeLine(d.y);
                for (k in 0..d.paths.length) {
                    Console.write("  ");
                    Console.writeLine(d.paths[k].view());
                }
                Console.flush();
                return 0;
            }
            null => {}
        }
    }
    say("timeout");
    return 1;
}

fn main() -> i32 {
    let mode = match (Process.argument(1)) {
        Some(m) => m,
        null => "trace",
    };
    let isReal = mode == "real";
    var a = match (Window.open("window_check drop", if (isReal) 300 else 40, if (isReal) 200 else 30)) {
        Ok(w) => w,
        Err(_) => { say("open failed"); return 1; }
    };
    if (isReal) {
        return real(mut a);
    }
    if (mode == "wait") {
        return waits(mut a);
    }
    return trace(mut a);
}
"""

# Touches (ADR 0155): with "trace", the script's fingers frame by frame, touches turned on before frame 2 and off
# before frame 6; with "full", 300 moves of one finger and then 200 taps at once; with "wait", a touch ending a wait
# alone; with "real", a real window's answer to setTouches.
TOUCH = r"""
fn say(text: StringView) {
    Console.writeLine(text);
    Console.flush();
}

fn verdict(what: StringView, ok: bool) {
    Console.write(what);
    Console.writeLine(if (ok) " ok" else " FAILED");
}

fn phaseName(p: WindowTouchPhase) -> StringView => match (p) {
    WindowTouchPhase.Began => "began",
    WindowTouchPhase.Moved => "moved",
    WindowTouchPhase.Ended => "ended",
    WindowTouchPhase.Cancelled => "cancelled",
};

fn events(window: borrow mut Window) {
    loop {
        match (window.nextEvent()) {
            Some(e) => {
                match (e) {
                    WindowEvent.PointerMoved(x, y) => {
                        Console.write("pointer ");
                        Console.write(x);
                        Console.write(" ");
                        Console.writeLine(y);
                    }
                    WindowEvent.PointerButton(_, down, x, y) => {
                        Console.write(if (down) "down " else "up ");
                        Console.write(x);
                        Console.write(" ");
                        Console.writeLine(y);
                    }
                    WindowEvent.Focus(f) => { Console.writeLine(if (f) "focus" else "no focus"); }
                    WindowEvent.Closed => { Console.writeLine("closed"); }
                    _ => { Console.writeLine("other"); }
                }
            }
            null => { break; }
        }
    }
}

fn touches(window: borrow mut Window) {
    loop {
        match (window.nextTouch()) {
            Some(t) => {
                Console.write("touch ");
                Console.write(t.id);
                Console.write(" ");
                Console.write(phaseName(t.phase));
                Console.write(" ");
                Console.write(t.x);
                Console.write(" ");
                Console.write(t.y);
                Console.write(" ");
                Console.writeLine(t.time / 16666667);
            }
            null => { break; }
        }
    }
}

fn drain(window: borrow mut Window) {
    loop {
        match (window.nextEvent()) {
            Some(_) => {}
            null => { break; }
        }
    }
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

/// The script's frames 0 to 7: before frame 2 touches are turned on, before frame 6 off; each frame's events, then its
/// touches.
fn trace(window: borrow mut Window) -> i32 {
    let pixels = [0u32; 1200];
    for (frame in 0..8) {
        if (frame == 2) {
            Console.write("on ");
            Console.writeLine(window.setTouches(true));
        }
        if (frame == 6) {
            Console.write("off ");
            Console.writeLine(window.setTouches(false));
        }
        events(window);
        touches(window);
        Console.write("frame ");
        Console.writeLine(frame);
        window.present(pixels[..], 40, 30)!;
    }
    return 0;
}

/// Frame 1's touches (one finger's 300 moves), then frame 2's (200 taps), summed up.
fn full(window: borrow mut Window) -> i32 {
    let pixels = [0u32; 1200];
    let _ = window.setTouches(true);
    for (frame in 0..3) {
        drain(window);
        var counts = [0u32; 4];
        var first: f32 = -1.0;
        var last: f32 = -1.0;
        var low: u32 = 0;
        var high: u32 = 0;
        loop {
            match (window.nextTouch()) {
                Some(t) => {
                    let k: usize = match (t.phase) {
                        WindowTouchPhase.Began => 0,
                        WindowTouchPhase.Moved => 1,
                        WindowTouchPhase.Ended => 2,
                        WindowTouchPhase.Cancelled => 3,
                    };
                    counts[k] += 1;
                    if (k == 1) {
                        if (first < 0.0) {
                            first = t.x;
                        }
                        last = t.x;
                    }
                    if (low == 0 || t.id < low) {
                        low = t.id;
                    }
                    if (t.id > high) {
                        high = t.id;
                    }
                }
                null => { break; }
            }
        }
        Console.write("frame ");
        Console.write(frame);
        Console.write(": ");
        Console.write(counts[0]);
        Console.write(" began, ");
        Console.write(counts[1]);
        Console.write(" moved, ");
        Console.write(counts[2]);
        Console.write(" ended, ");
        Console.write(counts[3]);
        Console.write(" cancelled");
        if (counts[1] > 0) {
            Console.write(", moves from ");
            Console.write(first);
            Console.write(" to ");
            Console.write(last);
        }
        if (high > 0) {
            Console.write(", ids ");
            Console.write(low);
            Console.write(" to ");
            Console.write(high);
        }
        Console.writeLine("");
        window.present(pixels[..], 40, 30)!;
    }
    return 0;
}

/// The script's one touch, at frame 1, ends a waitEvent of 20 s, and in a second window a waitAny of 20 s.
fn waits(a: borrow mut Window) -> i32 {
    let pixels = [0u32; 1200];
    let _ = a.setTouches(true);
    a.present(pixels[..], 40, 30)!;
    verdict("ends waitEvent", timed(a, 20000) < 5000);
    touches(a);
    verdict("then waitEvent waits", timed(a, 100) >= 90);
    var b = Window.open("window_check touch b", 40, 30)!;
    let _ = b.setTouches(true);
    drain(mut b);
    b.present(pixels[..], 40, 30)!;
    verdict("ends waitAny", timedAny(20000) < 5000);
    touches(mut b);
    verdict("then waitAny waits", timedAny(100) >= 90);
    return 0;
}

fn main() -> i32 {
    let mode = match (Process.argument(1)) {
        Some(m) => m,
        null => "trace",
    };
    var a = match (Window.open("window_check touch", 40, 30)) {
        Ok(w) => w,
        Err(_) => { say("open failed"); return 1; }
    };
    if (mode == "real") {
        Console.write("touches ");
        Console.writeLine(a.setTouches(true));
        return 0;
    }
    if (mode == "full") {
        return full(mut a);
    }
    if (mode == "wait") {
        return waits(mut a);
    }
    return trace(mut a);
}
"""

# The safe area (ADR 0156): the script's frames 0 to 5, each frame's events, then the safe area; with "real", a real
# window's safe area.
SAFE = r"""
fn insets(window: borrow Window) {
    let safe = window.safeArea();
    Console.write("safe ");
    Console.write(safe.left);
    Console.write(" ");
    Console.write(safe.top);
    Console.write(" ");
    Console.write(safe.right);
    Console.write(" ");
    Console.writeLine(safe.bottom);
}

fn main() -> i32 {
    var window = match (Window.open("window_check safe", 40, 30)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    if (Process.argument(1) == Some("real")) {
        insets(window);
        return 0;
    }
    let pixels = [0u32; 1800];
    for (frame in 0..6) {
        loop {
            match (window.nextEvent()) {
                Some(e) => {
                    match (e) {
                        WindowEvent.Resized(size) => {
                            Console.write("resized ");
                            Console.write(size.width);
                            Console.write(" ");
                            Console.writeLine(size.height);
                        }
                        WindowEvent.Closed => { Console.writeLine("closed"); }
                        _ => { Console.writeLine("other"); }
                    }
                }
                null => { break; }
            }
        }
        insets(window);
        Console.write("frame ");
        Console.writeLine(frame);
        let size = window.size();
        window.present(pixels[..(size.width * size.height) as usize], size.width, size.height)!;
    }
    return 0;
}
"""

# The soft keyboard (ADR 0157): the script's frames 0 to 4, each frame's events, then the keyboard's inset; with "real",
# a real window's inset.
KEYBOARD = r"""
fn inset(window: borrow Window) {
    Console.write("keyboard ");
    Console.writeLine(window.keyboardInset());
}

fn main() -> i32 {
    var window = match (Window.open("window_check keyboard", 40, 30)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    window.setTextInputKind(WindowTextKind.Email);
    window.setReturnKey(WindowReturnKey.Done);
    window.setTextInput(true);
    if (Process.argument(1) == Some("real")) {
        inset(window);
        return 0;
    }
    let pixels = [0u32; 1200];
    for (frame in 0..5) {
        loop {
            match (window.nextEvent()) {
                Some(e) => {
                    match (e) {
                        WindowEvent.Resized(size) => {
                            Console.write("resized ");
                            Console.write(size.width);
                            Console.write(" ");
                            Console.writeLine(size.height);
                        }
                        WindowEvent.Text(c) => {
                            Console.write("text ");
                            Console.writeLine(c);
                        }
                        WindowEvent.Closed => { Console.writeLine("closed"); }
                        _ => { Console.writeLine("other"); }
                    }
                }
                null => { break; }
            }
        }
        inset(window);
        Console.write("frame ");
        Console.writeLine(frame);
        window.present(pixels[..], 40, 30)!;
    }
    return 0;
}
"""

# The app's life (ADR 0159): the script's frames 0 to 4, each frame's app events; with "wait", an app event ending a
# wait; with "real", a real window's.
APP = r"""
fn events(window: borrow mut Window) {
    loop {
        match (window.nextAppEvent()) {
            Some(e) => {
                Console.writeLine(match (e) {
                    WindowAppEvent.Background => "background",
                    WindowAppEvent.Foreground => "foreground",
                    WindowAppEvent.MemoryWarning => "memory warning",
                });
            }
            null => { break; }
        }
    }
}

fn main() -> i32 {
    var window = match (Window.open("window_check app", 40, 30)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    let mode = Process.argument(1) ?? "trace";
    if (mode == "real") {
        events(mut window);
        Console.writeLine("no more");
        return 0;
    }
    let pixels = [0u32; 1200];
    if (mode == "wait") {
        window.present(pixels[..], 40, 30)!;
        let start = Timer.now();
        window.waitEvent(20000);
        Console.writeLine(if (Timer.now() - start < 5000) "ends waitEvent ok" else "ends waitEvent FAILED");
        events(mut window);
        return 0;
    }
    for (frame in 0..5) {
        events(mut window);
        Console.write("frame ");
        Console.writeLine(frame);
        window.present(pixels[..], 40, 30)!;
    }
    return 0;
}
"""

# Its script, and what APP prints for it: a foreground at first is none (a window opens there); a memory warning; the
# background, once; a warning there; the foreground; and the background again.
APP_SCRIPT = ("0:app:foreground;1:app:memory;2:app:background;2:app:background;2:app:memory;3:app:foreground;"
              "3:app:foreground;4:app:background")
APP_EXPECTED = ["frame 0", "memory warning", "frame 1", "background", "memory warning", "frame 2", "foreground",
                "frame 3", "background", "frame 4"]

# Exporting a file (ADR 0158): a draft written, exported to the script's answer, exported again with no answer, and a
# file that does not exist. Its argument is the folder to work in.
EXPORT = r"""
fn verdict(what: StringView, r: Result<bool, WindowError>) {
    Console.write(what);
    match (r) {
        Ok(saved) => { Console.writeLine(if (saved) " saved" else " cancelled"); }
        Err(e) => {
            Console.writeLine(match (e) {
                WindowError.Resources => " Resources",
                WindowError.Unsupported => " Unsupported",
                _ => " another error",
            });
        }
    }
}

fn main() -> i32 {
    var window = match (Window.open("window_check export", 40, 30)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    {
        var draft = File.create("draft.txt")!;
        draft.write("the draft's text".bytes())!;
    }
    verdict("first", FileDialog.exportFile(mut window, "Save As", "draft.txt", "notes.txt"));
    verdict("second", FileDialog.exportFile(mut window, "Save As", "draft.txt", "notes.txt"));
    verdict("missing", FileDialog.exportFile(mut window, "Save As", "no-such-file.txt", "notes.txt"));
    return 0;
}
"""

# Its script, and what KEYBOARD prints for it: none at first; a keyboard (a Resized at the same size) and a typed
# letter; the same again (nothing); one taller than the content is the content's height; none; the close.
KEYBOARD_SCRIPT = "1:keyboard:12.5;1:type:a;2:keyboard:12.5;3:keyboard:99;4:keyboard:0;4:close"
KEYBOARD_EXPECTED = ["keyboard 0", "frame 0",
                     "resized 40 30", "text 97", "keyboard 12.5", "frame 1",
                     "keyboard 12.5", "frame 2",
                     "resized 40 30", "keyboard 30", "frame 3",
                     "resized 40 30", "closed", "keyboard 0", "frame 4"]

# Accessibility (ADR 0164): a window with one element of each role. Scripted, it prints what takeAccessAction gives;
# "real", it also asks the system what the window holds (ACCESS_C, in this process: no permission is needed, as
# another process's questions would) and has it perform actions; "twice" and "outside" are the two panics.
ACCESS = r"""extern "C" {
    @effects()
    fn access_inspect(act: i32) -> i32;
}

fn node(id: u64, role: WindowAccessRole, flags: u32, y: f32, label: [u32; 2], value: [u32; 2]) -> WindowAccessNode =>
    WindowAccessNode {
        id: id,
        role: role,
        flags: flags,
        x: 10.0,
        y: y,
        width: 100.0,
        height: 18.0,
        min: 0.0,
        max: if (id == 6) 10.0 else 0.0,
        now: if (id == 6) 4.0 else 0.0,
        labelStart: label[0],
        labelLength: label[1],
        valueStart: value[0],
        valueLength: value[1],
        caret: if (id == 5) 1 else 0,
        anchor: if (id == 5) 2 else 0,
        layer: if (id == 2) 1 else 0,
        parent: 0,
        row: 0,
        column: 0,
        rows: 0,
        columns: 0,
    };

/// An element of the tree (ADR 0207): its container, its place, its counts.
fn held(id: u64, role: WindowAccessRole, parent: u64, label: [u32; 2], place: [u32; 2], counts: [u32; 2]) ->
    WindowAccessNode {
    var n = node(id, role, 0, 0.0, label, [0, 0]);
    n.y = round<f32>(id % 100) * 20.0;
    n.x = round<f32>(id / 100) * 10.0;
    n.parent = parent;
    n.row = place[0];
    n.column = place[1];
    n.rows = counts[0];
    n.columns = counts[1];
    return n;
}

fn tree(window: borrow mut Window) {
    let text = "FruitApplePearPlumEditCutCopyViewsOneTwoGridNameSizeKindA1xToolsGo";
    let none = [0u32, 0];
    let nodes = [
        // A list of 40 rows of which three are given, the program numbering them; the runtime numbers the rest.
        held(100, WindowAccessRole.List, 0, [0, 5], none, [40, 0]),
        held(101, WindowAccessRole.ListItem, 100, [5, 5], [3, 0], none),
        held(102, WindowAccessRole.ListItem, 100, [10, 4], [4, 0], none),
        held(103, WindowAccessRole.ListItem, 100, [14, 4], [5, 0], none),
        held(200, WindowAccessRole.Menu, 0, [18, 4], none, none),
        held(201, WindowAccessRole.MenuItem, 200, [22, 3], none, none),
        held(202, WindowAccessRole.MenuItem, 200, [25, 4], none, none),
        held(300, WindowAccessRole.TabList, 0, [29, 5], none, none),
        held(301, WindowAccessRole.Tab, 300, [34, 3], none, none),
        held(302, WindowAccessRole.Tab, 300, [37, 3], none, none),
        // A table of two rows and three columns, its first row the headers: all counted by the runtime.
        held(400, WindowAccessRole.Table, 0, [40, 4], none, none),
        held(410, WindowAccessRole.Row, 400, [0, 0], none, none),
        held(411, WindowAccessRole.ColumnHeader, 410, [44, 4], none, none),
        held(412, WindowAccessRole.ColumnHeader, 410, [48, 4], none, none),
        held(413, WindowAccessRole.ColumnHeader, 410, [52, 4], none, none),
        held(420, WindowAccessRole.Row, 400, [0, 0], none, none),
        held(421, WindowAccessRole.Cell, 420, [56, 1], none, none),
        held(422, WindowAccessRole.Cell, 420, [57, 1], none, none),
        held(423, WindowAccessRole.Cell, 420, [58, 1], none, none),
        held(500, WindowAccessRole.Group, 0, [59, 5], none, none),
        held(501, WindowAccessRole.Button, 500, [64, 2], none, none),
    ];
    window.setAccessNodes(nodes[..], text.bytes());
}

fn give(window: borrow mut Window, volume: f64, focusField: bool) {
    //          0123456789012345678901234567890123456789012345678901234
    let text = "HeadingSaveAgreeAppleNameVolumeTabRowItemPickIconAlPearOver";
    var nodes = [
        node(1, WindowAccessRole.Text, 0, 0.0, [0, 7], [0, 0]),
        node(2, WindowAccessRole.Button, 0, 20.0, [7, 4], [0, 0]),
        node(3, WindowAccessRole.Checkbox, WindowAccessNode.checked(), 40.0, [11, 5], [0, 0]),
        node(4, WindowAccessRole.Radio, 0, 60.0, [16, 5], [0, 0]),
        node(5, WindowAccessRole.TextField, if (focusField) WindowAccessNode.focused() else 0, 80.0, [21, 4], [49, 2]),
        node(6, WindowAccessRole.Slider, 0, 100.0, [25, 6], [0, 0]),
        node(7, WindowAccessRole.Tab, WindowAccessNode.selected(), 120.0, [31, 3], [0, 0]),
        node(8, WindowAccessRole.ListItem, 0, 140.0, [34, 3], [0, 0]),
        node(9, WindowAccessRole.MenuItem, WindowAccessNode.disabled(), 160.0, [37, 4], [0, 0]),
        node(10, WindowAccessRole.Dropdown, WindowAccessNode.expanded(), 180.0, [41, 4], [51, 4]),
        node(11, WindowAccessRole.Image, 0, 200.0, [45, 4], [0, 0]),
        // Later in the list and over Save, but of the page's layer, where Save is of layer 1 (ADR 0164 §10): the
        // point is Save's.
        node(12, WindowAccessRole.Text, 0, 20.0, [55, 4], [0, 0]),
    ];
    nodes[5].now = volume;
    window.setAccessNodes(nodes[..], text.bytes());
}

fn take(window: borrow mut Window) {
    loop {
        match (window.takeAccessAction()) {
            Some(r) => {
                Console.write("asked ");
                Console.write(r.id);
                Console.writeLine(match (r.action) {
                    WindowAccessAction.Press => " press",
                    WindowAccessAction.Focus => " focus",
                    WindowAccessAction.Increment => " increment",
                    WindowAccessAction.Decrement => " decrement",
                });
            }
            null => { break; }
        }
    }
}

/// Safety: the helper only asks AppKit about this process's window.
fn main() -> i32 {
    var window = match (Window.open("access check", 320, 240)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    let pixels = [0xFF202020u32; 76800];
    let real = Process.argument(1) == Some("real");
    if (Process.argument(1) == Some("twice")) {
        let pair = [
            node(7, WindowAccessRole.Button, 0, 0.0, [0, 1], [0, 0]),
            node(7, WindowAccessRole.Button, 0, 20.0, [0, 1], [0, 0]),
        ];
        window.setAccessNodes(pair[..], "x".bytes());
        return 0;
    }
    if (Process.argument(1) == Some("outside")) {
        let one = [node(7, WindowAccessRole.Button, 0, 0.0, [0, 2], [0, 0])];
        window.setAccessNodes(one[..], "x".bytes());
        return 0;
    }
    // A parent that is not in the list, comes after its child, or is no container (ADR 0207): panics.
    let none = [0u32, 0];
    if (Process.argument(1) == Some("unknown")) {
        let one = [held(7, WindowAccessRole.Button, 9, [0, 1], none, none)];
        window.setAccessNodes(one[..], "x".bytes());
        return 0;
    }
    if (Process.argument(1) == Some("later")) {
        let pair = [
            held(7, WindowAccessRole.ListItem, 8, [0, 1], none, none),
            held(8, WindowAccessRole.List, 0, [0, 1], none, none),
        ];
        window.setAccessNodes(pair[..], "x".bytes());
        return 0;
    }
    if (Process.argument(1) == Some("leaf")) {
        let pair = [
            held(7, WindowAccessRole.Button, 0, [0, 1], none, none),
            held(8, WindowAccessRole.ListItem, 7, [0, 1], none, none),
        ];
        window.setAccessNodes(pair[..], "x".bytes());
        return 0;
    }
    if (Process.argument(1) == Some("tree")) {
        tree(mut window);
        // "hold": the window stays about six seconds, for another process to ask the system (Windows' check).
        let hold = Process.argument(2) == Some("hold");
        for (frame in 0..(if (hold) 400 else 3)) {
            loop {
                match (window.nextEvent()) {
                    Some(_) => {}
                    null => { break; }
                }
            }
            match (window.present(pixels[..], 320, 240)) {
                Ok() => {}
                Err(_) => { Console.writeLine("present failed"); return 1; }
            }
            if (frame == 1 && !hold) {
                let top = unsafe { access_inspect(2) }; // its lines first: the C helper writes them itself
                Console.write("tree ");
                Console.writeLine(top);
            }
            window.waitFrame();
        }
        return 0;
    }
    Console.write("active ");
    Console.writeLine(window.accessActive());
    give(mut window, 4.0, false);
    for (frame in 0..6) {
        loop {
            match (window.nextEvent()) {
                Some(_) => {}
                null => { break; }
            }
        }
        take(mut window);
        match (window.present(pixels[..], 320, 240)) {
            Ok() => {}
            Err(_) => { Console.writeLine("present failed"); return 1; }
        }
        if (real && frame == 1) {
            Console.write("inspect ");
            Console.writeLine(unsafe { access_inspect(1) });
            Console.write("active ");
            Console.writeLine(window.accessActive());
        }
        if (real && frame == 3) {
            give(mut window, 7.0, true);
            Console.write("inspect ");
            Console.writeLine(unsafe { access_inspect(0) });
            window.announce("Saved");
        }
        window.waitFrame();
    }
    return 0;
}
"""

ACCESS_C = r"""/* Asks AppKit, in this process, what the key window's content view holds for a screen reader. */
#include <stdint.h>
#include <stdio.h>
#if defined(__APPLE__)
#include <dlfcn.h>
typedef void *id;
typedef void *SEL;
typedef void *Class;
static void *sym(const char *name) { return dlsym(RTLD_DEFAULT, name); }
#define objc_msgSend (sym("objc_msgSend"))
static SEL sel_registerName(const char *n) { return ((SEL(*)(const char *))sym("sel_registerName"))(n); }
static Class objc_getClass(const char *n) { return ((Class(*)(const char *))sym("objc_getClass"))(n); }
typedef struct { double x, y, w, h; } rect;
static id send0(id o, const char *s) { return ((id(*)(id, SEL))objc_msgSend)(o, sel_registerName(s)); }
static const char *text(id s) {
    if (s == NULL) return "-";
    if (!((signed char(*)(id, SEL, Class))objc_msgSend)(s, sel_registerName("isKindOfClass:"), objc_getClass("NSString")))
        s = send0(s, "description");
    return ((const char *(*)(id, SEL))objc_msgSend)(s, sel_registerName("UTF8String"));
}
static void *seen[32];
typedef struct { unsigned long location, length; } nsrange;
static long count_of(id array) { return array == NULL ? 0 : ((long (*)(id, SEL))objc_msgSend)(array, sel_registerName("count")); }
static id item(id array, long k) { return ((id(*)(id, SEL, unsigned long))objc_msgSend)(array, sel_registerName("objectAtIndex:"), (unsigned long)k); }
static long number(id e, const char *s) { return ((long (*)(id, SEL))objc_msgSend)(e, sel_registerName(s)); }
/* The tree under `up` (ADR 0207): each element's role, label, place and counts, and whether its parent is `up`. */
static void walk(id up, int depth) {
    id children = send0(up, "accessibilityChildren");
    for (long k = 0; k < count_of(children); k++) {
        id e = item(children, k);
        const char *role = text(send0(e, "accessibilityRole"));
        nsrange r = ((nsrange(*)(id, SEL))objc_msgSend)(e, sel_registerName("accessibilityRowIndexRange"));
        nsrange c = ((nsrange(*)(id, SEL))objc_msgSend)(e, sel_registerName("accessibilityColumnIndexRange"));
        printf("  %*s%s %s index=%ld rows=%ld columns=%ld cell=%lu,%lu up=%d", depth * 2, "", role,
               text(send0(e, "accessibilityLabel")), number(e, "accessibilityIndex"), number(e, "accessibilityRowCount"),
               number(e, "accessibilityColumnCount"), r.location, c.location, send0(e, "accessibilityParent") == up);
        id headers = send0(e, "accessibilityColumnHeaderUIElements");
        if (count_of(headers) > 0) {
            printf(" headers=");
            for (long h = 0; h < count_of(headers); h++) printf("%s%s", h ? "," : "", text(send0(item(headers, h), "accessibilityLabel")));
        }
        printf("\n");
        walk(e, depth + 1);
    }
}
int32_t access_inspect(int32_t act);
int32_t access_inspect(int32_t act) {
    id app = send0((id)objc_getClass("NSApplication"), "sharedApplication");
    id windows = send0(app, "windows");
    id window = ((id(*)(id, SEL, unsigned long))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), 0ul);
    id view = send0(window, "contentView");
    id children = send0(view, "accessibilityChildren");
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(children, sel_registerName("count"));
    if (act == 2) {
        walk(view, 0);
        fflush(stdout);
        return (int32_t)n;
    }
    rect wf = ((rect(*)(id, SEL))objc_msgSend)(window, sel_registerName("frame"));
    rect cr = ((rect(*)(id, SEL, rect))objc_msgSend)(window, sel_registerName("contentRectForFrameRect:"), wf);
    int same = 0;
    rect save = {0, 0, 0, 0};
    for (unsigned long k = 0; k < n; k++) {
        id e = ((id(*)(id, SEL, unsigned long))objc_msgSend)(children, sel_registerName("objectAtIndex:"), k);
        rect f = ((rect(*)(id, SEL))objc_msgSend)(e, sel_registerName("accessibilityFrame"));
        signed char enabled = ((signed char (*)(id, SEL))objc_msgSend)(e, sel_registerName("isAccessibilityEnabled"));
        signed char selected = ((signed char (*)(id, SEL))objc_msgSend)(e, sel_registerName("isAccessibilitySelected"));
        signed char focused = ((signed char (*)(id, SEL))objc_msgSend)(e, sel_registerName("isAccessibilityFocused"));
        printf("  %s %s label=%s value=%s at %.0f,%.0f %.0fx%.0f enabled=%d selected=%d focused=%d parent=%d\n",
               text(send0(e, "accessibilityRole")), text(send0(e, "accessibilitySubrole")),
               text(send0(e, "accessibilityLabel")), text(send0(e, "accessibilityValue")), f.x - cr.x,
               cr.y + cr.h - f.y - f.h, f.w, f.h, enabled, selected, focused, send0(e, "accessibilityParent") == view);
        if (k == 4) { /* the text field: its selection, from the node's caret and anchor */
            typedef struct { unsigned long location, length; } range;
            range r = ((range(*)(id, SEL))objc_msgSend)(e, sel_registerName("accessibilitySelectedTextRange"));
            printf("  selection %lu+%lu %s of %ld\n", r.location, r.length, text(send0(e, "accessibilitySelectedText")),
                   ((long (*)(id, SEL))objc_msgSend)(e, sel_registerName("accessibilityNumberOfCharacters")));
        }
        if (k == 1) save = f;
        if (k < 32) {
            same += seen[k] == (void *)e;
            seen[k] = (void *)e;
        }
        if (act && k == 1) ((signed char (*)(id, SEL))objc_msgSend)(e, sel_registerName("accessibilityPerformPress"));
        if (act && k == 5) ((signed char (*)(id, SEL))objc_msgSend)(e, sel_registerName("accessibilityPerformIncrement"));
        if (act && k == 5) ((signed char (*)(id, SEL))objc_msgSend)(e, sel_registerName("accessibilityPerformDecrement"));
        if (act && k == 4) ((void (*)(id, SEL, signed char))objc_msgSend)(e, sel_registerName("setAccessibilityFocused:"), 1);
        if (act && k == 8) ((signed char (*)(id, SEL))objc_msgSend)(e, sel_registerName("accessibilityPerformPress"));
    }
    /* What is under Save's middle, which a later element of a lower layer covers too: the higher layer's. */
    typedef struct { double x, y; } point;
    point middle = {save.x + save.w / 2, save.y + save.h / 2};
    id hit = ((id(*)(id, SEL, point))objc_msgSend)(view, sel_registerName("accessibilityHitTest:"), middle);
    printf("  hit %s %s\n", text(send0(hit, "accessibilityRole")), text(send0(hit, "accessibilityLabel")));
    printf("  same elements as last time: %d\n", same);
    fflush(stdout);
    return (int32_t)n;
}
#else
int32_t access_inspect(int32_t act) { (void)act; return -1; }
#endif
"""

# Requests of elements the list has, in order; of one it has not (99), nothing. Frame 0's items are taken when the
# window opens, before the program gave a list, so the script starts at frame 1.
ACCESS_SCRIPT = ("1:access:2,press;1:access:6,increment;2:access:5,focus;2:access:99,press;3:access:6,decrement;"
                 "3:access:9,press;5:close")
ACCESS_EXPECTED = ["active false", "asked 2 press", "asked 6 increment", "asked 5 focus", "asked 6 decrement",
                   "asked 9 press"]


# A resize that comes inside the program's wait with no event behind it (another program's: an accessibility
# client's, a window manager's, a script's) ends that wait with Resized, as an input does. RESIZE_C resizes the window
# from a run loop timer, in AppKit's wait, as such a resize arrives; the wait is three seconds.
RESIZE = r"""extern "C" {
    @effects()
    fn resize_later(width: f64, height: f64) -> i32;
}

/// Safety: the helper only resizes this process's window, from its run loop.
fn main() -> i32 {
    var window = match (Window.open("resize check", 320, 240)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    let pixels = [0xFF202020u32; 76800];
    // What opening it brings (the focus, the first size) is taken first.
    for (frame in 0..3) {
        match (window.present(pixels[..], 320, 240)) {
            Ok() => {}
            Err(_) => { Console.writeLine("present failed"); return 1; }
        }
        window.waitEvent(150);
        loop {
            match (window.nextEvent()) {
                Some(_) => {}
                null => { break; }
            }
        }
    }
    if (unsafe { resize_later(200.0, 150.0) } != 1) {
        Console.writeLine("no timer");
        return 1;
    }
    let start = Timer.now();
    var found = false;
    while (!found && Timer.now() - start < 3000) {
        window.waitEvent(truncate<u32>(3000 - (Timer.now() - start).min(3000)));
        loop {
            match (window.nextEvent()) {
                Some(WindowEvent.Resized(size)) => { found = found || (size.width == 200 && size.height == 150); }
                Some(_) => {}
                null => { break; }
            }
        }
    }
    let waited = Timer.now() - start;
    Console.writeLine(if (!found) "no resize" else if (waited < 1500) "resized in the wait" else "resized late");
    return 0;
}
"""

RESIZE_C = r"""/* Resizes the key window's content from a run loop timer: inside AppKit's wait, with no event behind it. */
#include <stddef.h>
#include <stdint.h>
#if defined(__APPLE__)
#include <dlfcn.h>
typedef void *id;
typedef void *SEL;
static void *sym(const char *name) { return dlsym(RTLD_DEFAULT, name); }
static SEL sel(const char *n) { return ((SEL(*)(const char *))sym("sel_registerName"))(n); }
static id cls(const char *n) { return ((id(*)(const char *))sym("objc_getClass"))(n); }
typedef struct { double w, h; } size;
static size wanted;
static void fire(void *timer, void *info) {
    (void)timer;
    (void)info;
    void *send = sym("objc_msgSend");
    id app = ((id(*)(id, SEL))send)(cls("NSApplication"), sel("sharedApplication"));
    id windows = ((id(*)(id, SEL))send)(app, sel("windows"));
    id window = ((id(*)(id, SEL, unsigned long))send)(windows, sel("objectAtIndex:"), 0ul);
    ((void (*)(id, SEL, size))send)(window, sel("setContentSize:"), wanted);
}
int32_t resize_later(double width, double height);
int32_t resize_later(double width, double height) {
    wanted.w = width;
    wanted.h = height;
    void *(*create)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *) =
        (void *(*)(void *, double, double, unsigned long, long, void (*)(void *, void *), void *))sym("CFRunLoopTimerCreate");
    double (*now)(void) = (double (*)(void))sym("CFAbsoluteTimeGetCurrent");
    void *(*main_loop)(void) = (void *(*)(void))sym("CFRunLoopGetMain");
    void (*add)(void *, void *, void *) = (void (*)(void *, void *, void *))sym("CFRunLoopAddTimer");
    void **common = (void **)sym("kCFRunLoopCommonModes");
    if (create == NULL || now == NULL || main_loop == NULL || add == NULL || common == NULL) return 0;
    void *timer = create(NULL, now() + 0.3, 0.0, 0, 0, fire, NULL);
    if (timer == NULL) return 0;
    add(main_loop(), timer, *common);
    return 1;
}
#else
int32_t resize_later(double width, double height) { (void)width; (void)height; return 0; }
#endif
"""


# What ACCESS prints with "tree" on macOS (ADR 0207): the containers with their children, places and counts, as
# AppKit reports them. A list's item says its place from the program (3 of 40); the rest are numbered by the runtime.
def _tree_line(depth, role, label, index=0, rows=0, columns=0, cell=(0, 0), headers=None):
    line = f"  {'  ' * depth}{role} {label} index={index} rows={rows} columns={columns} cell={cell[0]},{cell[1]} up=1"
    return line + (f" headers={headers}" if headers else "")


ACCESS_TREE_EXPECTED = [
    _tree_line(0, "AXList", "Fruit", rows=40),
    _tree_line(1, "AXButton", "Apple", index=2), _tree_line(1, "AXButton", "Pear", index=3),
    _tree_line(1, "AXButton", "Plum", index=4),
    _tree_line(0, "AXMenu", "Edit"),
    _tree_line(1, "AXMenuItem", "Cut", index=0), _tree_line(1, "AXMenuItem", "Copy", index=1),
    _tree_line(0, "AXTabGroup", "Views"),
    _tree_line(1, "AXRadioButton", "One", index=0), _tree_line(1, "AXRadioButton", "Two", index=1),
    _tree_line(0, "AXTable", "Grid", rows=2, columns=3, headers="Name,Size,Kind"),
    _tree_line(1, "AXRow", "", index=0),
    _tree_line(2, "AXCell", "Name", cell=(0, 0)), _tree_line(2, "AXCell", "Size", cell=(0, 1)),
    _tree_line(2, "AXCell", "Kind", cell=(0, 2)),
    _tree_line(1, "AXRow", "", index=1),
    _tree_line(2, "AXCell", "A", cell=(1, 0)), _tree_line(2, "AXCell", "1", cell=(1, 1)),
    _tree_line(2, "AXCell", "x", cell=(1, 2)),
    _tree_line(0, "AXGroup", "Tools"),
    _tree_line(1, "AXButton", "Go", index=0),
    "tree 5",
]


def access_real_expected():
    """What ACCESS prints with "real" on macOS: the list as AppKit reports it, the actions, the changed list."""
    rows = [("AXStaticText -", "-", "Heading", 1, 0), ("AXButton -", "Save", "-", 1, 0),
            ("AXCheckBox -", "Agree", "1", 1, 0), ("AXRadioButton -", "Apple", "0", 1, 0),
            ("AXTextField -", "Name", "Al", 1, 0), ("AXSlider -", "Volume", "4", 1, 0),
            ("AXRadioButton AXTabButton", "Tab", "1", 1, 1), ("AXButton -", "Row", "-", 1, 0),
            ("AXMenuItem -", "Item", "-", 0, 0), ("AXPopUpButton -", "Pick", "Pear", 1, 0),
            ("AXImage -", "Icon", "-", 1, 0), ("AXStaticText -", "-", "Over", 1, 0)]

    def listing(volume, focused, same):
        out = []
        for k, (role, label, value, enabled, selected) in enumerate(rows):
            value = volume if label == "Volume" else value
            y = 20 if value == "Over" else 20 * k  # Over lies on Save
            line = (f"  {role} label={label} value={value} at 10,{y} 100x18 enabled={enabled} "
                    f"selected={selected} focused={1 if focused and label == 'Name' else 0} parent=1")
            out.append(("inspect " if k == 0 else "") + line)
            if label == "Name":
                out.append("  selection 1+1 l of 2")
        return out + ["  hit AXButton Save", f"  same elements as last time: {same}", "12"]

    return (["active false"] + listing("4", False, 0) + ["active true", "asked 2 press", "asked 5 focus",
            "asked 6 increment", "asked 6 decrement"] + listing("7", True, 12))


# Its script, and what SAFE prints for it: none at first; a phone's portrait insets (a Resized at the same size); the
# same again (nothing); a rotation with new insets (the resize's Resized, then the safe area's); none again; the close.
SAFE_SCRIPT = "1:safe:0,47,0,34;2:safe:0,47,0,34;3:resize:60,30;3:safe:47.5,0,47.5,21;4:safe:0,0,0,0;5:close"
SAFE_EXPECTED = ["safe 0 0 0 0", "frame 0",
                 "resized 40 30", "safe 0 47 0 34", "frame 1",
                 "safe 0 47 0 34", "frame 2",
                 "resized 60 30", "resized 60 30", "safe 47.5 0 47.5 21", "frame 3",
                 "resized 60 30", "safe 0 0 0 0", "frame 4",
                 "closed", "safe 0 0 0 0", "frame 5"]

# The trace's script, and what TOUCH prints for it. Touches off: finger 1 is the pointer, finger 2 is ignored, and
# finger 3 is the pointer next. On (frame 2): finger 3 lifts the pointer and is no touch; finger 4 is touch 1, finger 5
# touch 2 (a move to the same place is none, a move before an end that moved); the script's finger 4 again is touch 3;
# a cancel at the last place; focus lost cancels touch 3; touch 4 is cancelled by turning touches off; finger 7 is the
# pointer again; closing ends nothing more.
TOUCH_SCRIPT = ("0:touch:1,began,10,10;0:touch:1,moved,12,10;1:touch:2,began,20,20;1:touch:1,ended,12,11;"
                "1:touch:2,ended,20,20;1:touch:3,began,5,5;2:touch:3,moved,6,6;2:touch:3,ended,6,6;"
                "2:touch:4,began,20,20;3:touch:5,began,30,30;3:touch:4,moved,21,22;3:touch:4,moved,21,22;"
                "4:touch:4,ended,25,26;4:touch:4,began,40,40;4:touch:5,cancelled,99,99;5:focus:0;"
                "5:touch:6,began,1,1;6:touch:7,began,2,2;6:touch:7,moved,3,2;7:close")
TOUCH_EXPECTED = ["pointer 10 10", "down 10 10", "pointer 12 10", "frame 0",
                  "up 12 11", "pointer 5 5", "down 5 5", "frame 1",
                  "on true", "up 5 5", "touch 1 began 20 20 2", "frame 2",
                  "touch 2 began 30 30 3", "touch 1 moved 21 22 3", "frame 3",
                  "touch 1 moved 25 26 4", "touch 1 ended 25 26 4", "touch 3 began 40 40 4",
                  "touch 2 cancelled 30 30 4", "frame 4",
                  "no focus", "touch 3 cancelled 40 40 4", "touch 4 began 1 1 5", "frame 5",
                  "off true", "pointer 2 2", "down 2 2", "pointer 3 2", "touch 4 cancelled 1 1 5", "frame 6",
                  "closed", "frame 7"]
TOUCH_FULL_SCRIPT = (";".join(["1:touch:1,began,0,0"] + [f"1:touch:1,moved,{k},0" for k in range(1, 301)] +
                              ["1:touch:1,ended,300,0"]) + ";" +
                     ";".join(f"2:touch:{k},began,{k},1;2:touch:{k},ended,{k},1" for k in range(10, 210)))
TOUCH_FULL_EXPECTED = ["frame 0: 0 began, 0 moved, 0 ended, 0 cancelled",
                       "frame 1: 1 began, 254 moved, 1 ended, 0 cancelled, moves from 2 to 300, ids 1 to 1",
                       "frame 2: 128 began, 0 moved, 128 ended, 0 cancelled, ids 74 to 201"]
TOUCH_WAIT = ["ends waitEvent ok", "touch 1 began 2 3 1", "then waitEvent waits ok", "ends waitAny ok",
              "touch 1 began 2 3 1", "then waitAny waits ok"]

# The trace's script (bytes: two paths are not UTF-8), and what DROP prints for it: a drag at frame 0; a drag that
# moves and leaves in frame 1; one that is dropped in frame 2 (the drop ends it); a drop with a path that is not UTF-8
# (left out) and one with only such a path (skipped); a drag at frame 5, which stays.
DROP_SCRIPT = (b"0:dragover:10,20:2;1:dragover:12.5,21:2;1:dragleave;2:dragover:5,6:1;"
               b"2:drop:5,7:/tmp/a b.txt|/tmp/\xc3\xbc.txt|;3:drop:1.5,2:/bad\xff|/ok;4:drop:3,4:/only\xfe;"
               b"5:dragover:7,8:3")
DROP_EXPECTED = ["over 10 20 2", "no drop",
                 "over none", "no drop", "over none",
                 "over none", "drop 5 7", "  /tmp/a b.txt", "  /tmp/ü.txt", "over none",
                 "over none", "drop 1.5 2", "  /ok", "over none",
                 "over none", "no drop", "over none",
                 "over 7 8 3", "no drop", "over 7 8 3"]
DROP_WAIT_DRAG = ["ends waitEvent ok", "over 7 8 3", "no drop", "then waitEvent waits ok", "ends waitAny ok",
                  "over 7 8 3", "no drop", "then waitAny waits ok"]
DROP_WAIT_DROP = ["ends waitEvent ok", "over none", "drop 2 3", "  /w", "then waitEvent waits ok", "ends waitAny ok",
                  "over none", "drop 2 3", "  /w", "then waitAny waits ok"]

# A person's drag on macOS (ADR 0150, --display): a small window of this helper, a press on it that starts a dragging
# session of files (or text), the pointer moved to the target point in steps by the system's mouse events, and a
# release there, or with `leave` back over the helper. It prints the operation the session ended with.
MAC_DRAG_SOURCE = r"""/* window_check's drag source on macOS (ADR 0150).
   usage: dragsource drop|leave|text PID WIDTH HEIGHT X Y [PATH...]
   PID's window's content is WIDTH x HEIGHT points; X, Y is the target point in that content. */
#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static NSString *mode;
static NSArray<NSString *> *paths;
static CGPoint start, target;

@interface Source : NSView <NSDraggingSource>
@end

@implementation Source
- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    (void)event;
    return YES;
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [[NSColor systemOrangeColor] setFill];
    NSRectFill(self.bounds);
}

- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    (void)context;
    return NSDragOperationCopy;
}

- (void)mouseDown:(NSEvent *)event {
    NSImage *image = [NSImage imageNamed:NSImageNameMultipleDocuments];
    NSMutableArray *writers = [NSMutableArray array];
    if ([mode isEqualToString:@"text"]) {
        [writers addObject:@"window_check text"];
    } else {
        for (NSString *p in paths) {
            [writers addObject:[NSURL fileURLWithPath:p]];
        }
    }
    NSMutableArray *items = [NSMutableArray array];
    for (id writer in writers) {
        NSDraggingItem *item = [[NSDraggingItem alloc] initWithPasteboardWriter:writer];
        [item setDraggingFrame:NSMakeRect(0, 0, 32, 32) contents:image];
        [items addObject:item];
    }
    [self beginDraggingSessionWithItems:items event:event source:self];
}

- (void)draggingSession:(NSDraggingSession *)session
           endedAtPoint:(NSPoint)point
              operation:(NSDragOperation)operation {
    (void)session;
    (void)point;
    printf("operation %lu\n", (unsigned long)operation);
    fflush(stdout);
    exit(0);
}
@end

static void post(CGEventType type, CGPoint at) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, type, at, kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

static void glide(CGPoint a, CGPoint b) {
    for (int k = 1; k <= 40; k++) {
        post(kCGEventLeftMouseDragged, CGPointMake(a.x + (b.x - a.x) * k / 40.0, a.y + (b.y - a.y) * k / 40.0));
        usleep(15000);
    }
}

static void drive(void) {
    post(kCGEventMouseMoved, start);
    usleep(400000);
    post(kCGEventLeftMouseDown, start);
    usleep(300000);
    glide(start, target);
    for (int k = 0; k < 10; k++) {
        post(kCGEventLeftMouseDragged, CGPointMake(target.x + (k % 2), target.y));
        usleep(50000);
    }
    post(kCGEventLeftMouseDragged, target);
    usleep(300000);
    if ([mode isEqualToString:@"leave"]) {
        glide(target, start);
        usleep(300000);
        post(kCGEventLeftMouseUp, start);
    } else {
        post(kCGEventLeftMouseUp, target);
    }
    sleep(10);
    printf("timeout\n");
    fflush(stdout);
    exit(2);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 7) {
            fprintf(stderr, "usage: dragsource drop|leave|text PID WIDTH HEIGHT X Y [PATH...]\n");
            return 2;
        }
        mode = [NSString stringWithUTF8String:argv[1]];
        pid_t pid = (pid_t)atoi(argv[2]);
        double height = atof(argv[4]), x = atof(argv[5]), y = atof(argv[6]);
        NSMutableArray *list = [NSMutableArray array];
        for (int k = 7; k < argc; k++) {
            [list addObject:[NSString stringWithUTF8String:argv[k]]];
        }
        paths = list;
        CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
        CGRect bounds = CGRectNull;
        for (CFIndex k = 0; windows != NULL && k < CFArrayGetCount(windows); k++) {
            NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(windows, k);
            if ([info[(id)kCGWindowOwnerPID] intValue] == pid && [info[(id)kCGWindowLayer] intValue] == 0) {
                CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(id)kCGWindowBounds], &bounds);
                break;
            }
        }
        if (windows != NULL) {
            CFRelease(windows);
        }
        if (CGRectIsNull(bounds)) {
            printf("no window\n");
            return 2;
        }
        target = CGPointMake(bounds.origin.x + x, bounds.origin.y + bounds.size.height - height + y);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSRect screen = [[NSScreen screens][0] frame];
        NSRect frame = NSMakeRect(screen.origin.x + 60, screen.origin.y + screen.size.height - 260, 120, 120);
        NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        [window setLevel:NSFloatingWindowLevel];
        [window setContentView:[[Source alloc] initWithFrame:NSMakeRect(0, 0, 120, 120)]];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        NSRect content = [window convertRectToScreen:[[window contentView] frame]];
        start = CGPointMake(NSMidX(content), screen.size.height - NSMidY(content));
        [NSThread detachNewThreadWithBlock:^{
          drive();
        }];
        [NSApp run];
    }
    return 0;
}
"""

# A file manager's drag on X11 (ADR 0150, --display): an XDND version 5 source that owns XdndSelection, sends
# XdndEnter (`drop`: four types through XdndTypeList; `leave`: three; `text`: no text/uri-list), three XdndPositions
# ending at the target point, answers the conversion to text/uri-list (the paths, a comment, and three URIs that are
# not local files: http, a malformed escape, another host's), and sends XdndLeave or XdndDrop. It prints the target's
# XdndAware version, each XdndStatus's accept bit, and the drop's XdndFinished.
XDND_SOURCE = r"""/* window_check's XDND source on X11 (ADR 0150).
   usage: xdndsource drop|leave|text WINDOW X Y [PATH...] */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

static Display *display;
static Window source, target;
static Atom aware, enter, position, status, leave, drop, finished, selection, copy, type_list, uri_list, text_plain,
    utf8, other;
static char list[65536];
static size_t list_length;

static void send(Atom type, long a, long b, long c, long d) {
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = target;
    e.xclient.message_type = type;
    e.xclient.format = 32;
    e.xclient.data.l[0] = (long)source;
    e.xclient.data.l[1] = a;
    e.xclient.data.l[2] = b;
    e.xclient.data.l[3] = c;
    e.xclient.data.l[4] = d;
    XSendEvent(display, target, False, NoEventMask, &e);
    XFlush(display);
}

static void answer(XSelectionRequestEvent *r) {
    XEvent reply;
    memset(&reply, 0, sizeof reply);
    reply.xselection.type = SelectionNotify;
    reply.xselection.requestor = r->requestor;
    reply.xselection.selection = r->selection;
    reply.xselection.target = r->target;
    reply.xselection.time = r->time;
    reply.xselection.property = None;
    if (r->selection == selection && r->target == uri_list && r->property != None) {
        XChangeProperty(display, r->requestor, r->property, uri_list, 8, PropModeReplace, (unsigned char *)list,
                        (int)list_length);
        reply.xselection.property = r->property;
        printf("converted\n");
    }
    XSendEvent(display, r->requestor, False, NoEventMask, &reply);
    XFlush(display);
}

/* Takes the events for `ms` milliseconds, answering conversions; with `type`, until a client message of it. */
static int take(Atom type, int ms, XEvent *out) {
    for (int k = 0; k < ms / 10; k++) {
        while (XPending(display) > 0) {
            XEvent e;
            XNextEvent(display, &e);
            if (e.type == SelectionRequest) {
                answer(&e.xselectionrequest);
            } else if (type != None && e.type == ClientMessage && e.xclient.message_type == type) {
                *out = e;
                return 1;
            }
        }
        fd_set set;
        FD_ZERO(&set);
        FD_SET(ConnectionNumber(display), &set);
        struct timeval t = {0, 10000};
        select(ConnectionNumber(display) + 1, &set, NULL, NULL, &t);
    }
    return 0;
}

static void move_to(int x, int y) {
    int rx = 0, ry = 0;
    Window child;
    XTranslateCoordinates(display, target, DefaultRootWindow(display), x, y, &rx, &ry, &child);
    send(position, 0, ((long)rx << 16) | (long)(ry & 0xffff), CurrentTime, (long)copy);
    XEvent e;
    if (take(status, 5000, &e)) {
        printf("status %ld\n", e.xclient.data.l[1] & 1);
    } else {
        printf("no status\n");
    }
    take(None, 300, &e);
}

static void add(const char *text) {
    size_t n = strlen(text);
    if (list_length + n < sizeof list) {
        memcpy(list + list_length, text, n);
        list_length += n;
    }
}

static void add_file(const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    char uri[4096 * 3 + 16];
    size_t n = 0;
    memcpy(uri, "file://", 7);
    n = 7;
    for (const unsigned char *p = (const unsigned char *)path; *p != 0 && n + 6 < sizeof uri; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/-_.~", *p)) {
            uri[n++] = (char)*p;
        } else {
            uri[n++] = '%';
            uri[n++] = hex[*p >> 4];
            uri[n++] = hex[*p & 15];
        }
    }
    memcpy(uri + n, "\r\n", 3);
    add(uri);
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: xdndsource drop|leave|text WINDOW X Y [PATH...]\n");
        return 2;
    }
    const char *mode = argv[1];
    target = (Window)strtoul(argv[2], NULL, 0);
    int x = atoi(argv[3]), y = atoi(argv[4]);
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        printf("no display\n");
        return 2;
    }
    source = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    aware = XInternAtom(display, "XdndAware", False);
    enter = XInternAtom(display, "XdndEnter", False);
    position = XInternAtom(display, "XdndPosition", False);
    status = XInternAtom(display, "XdndStatus", False);
    leave = XInternAtom(display, "XdndLeave", False);
    drop = XInternAtom(display, "XdndDrop", False);
    finished = XInternAtom(display, "XdndFinished", False);
    selection = XInternAtom(display, "XdndSelection", False);
    copy = XInternAtom(display, "XdndActionCopy", False);
    type_list = XInternAtom(display, "XdndTypeList", False);
    uri_list = XInternAtom(display, "text/uri-list", False);
    text_plain = XInternAtom(display, "text/plain", False);
    utf8 = XInternAtom(display, "UTF8_STRING", False);
    other = XInternAtom(display, "application/x-window-check", False);
    add("# window_check\r\n");
    for (int k = 5; k < argc; k++) {
        add_file(argv[k]);
    }
    add("http://example.com/not-a-file\r\n");
    add("file:///window_check%zz\r\n");
    add("file://elsewhere.invalid/window_check\r\n");
    Atom type = None;
    int format = 0;
    unsigned long items = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(display, target, aware, 0, 1, False, XA_ATOM, &type, &format, &items, &after, &data) ==
            Success &&
        data != NULL && items == 1) {
        printf("aware %lu\n", *(unsigned long *)data);
    } else {
        printf("not aware\n");
    }
    if (data != NULL) {
        XFree(data);
    }
    XSetSelectionOwner(display, selection, source, CurrentTime);
    if (strcmp(mode, "drop") == 0) {
        Atom types[4] = {other, text_plain, uri_list, utf8};
        XChangeProperty(display, source, type_list, XA_ATOM, 32, PropModeReplace, (unsigned char *)types, 4);
        send(enter, (5L << 24) | 1, (long)types[0], (long)types[1], (long)types[2]);
    } else if (strcmp(mode, "leave") == 0) {
        send(enter, 5L << 24, (long)text_plain, (long)uri_list, 0);
    } else {
        send(enter, 5L << 24, (long)text_plain, (long)utf8, 0);
    }
    move_to(x - 30, y - 20);
    move_to(x - 10, y - 5);
    move_to(x, y);
    if (strcmp(mode, "drop") == 0) {
        send(drop, 0, CurrentTime, 0, 0);
        XEvent e;
        if (take(finished, 5000, &e)) {
            printf("finished %ld %s\n", e.xclient.data.l[1] & 1, (Atom)e.xclient.data.l[2] == copy ? "copy" : "none");
        } else {
            printf("no finished\n");
        }
    } else {
        send(leave, 0, 0, 0, 0);
        printf("left\n");
    }
    fflush(stdout);
    XCloseDisplay(display);
    return 0;
}
"""

# A person's drag on Windows (ADR 0150, --display): a press on this helper's small window starts an OLE drag
# (DoDragDrop) of files (CF_HDROP) or text (CF_UNICODETEXT), and the system's mouse input (SendInput) moves the pointer
# to the target point in steps and releases it there, or with `leave` back over the helper. It prints the effect.
# Windows (ADR 0207): what UI Automation's clients see of the "tree" window, asked from another process (a client on
# the window's own thread would wait on the providers it asks). Each element of the program's (FrameworkId CeeSafe):
# its control type, name, place in its set, a table's grid and column headers, a cell's place.
WIN_UIA_TREE = r"""#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>
#include <stdio.h>
/* The interfaces' and the class's GUIDs, as UIAutomationClient.h has them: defined here, so that the helper links
   alike with MinGW and with clang over Visual Studio's libraries. */
static const CLSID cuia = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
static const IID iuia = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
static const IID igrid = {0x414c3cdc, 0x856b, 0x4f5b, {0x85, 0x38, 0x31, 0x31, 0xc6, 0x30, 0x25, 0x50}};
static const IID itable = {0x620e691c, 0xea96, 0x4710, {0xa8, 0x50, 0x75, 0x4b, 0x24, 0xce, 0x24, 0x17}};
static const IID icell = {0x78f8ef57, 0x66c3, 0x4e09, {0xbd, 0x7c, 0xe7, 0x9b, 0x20, 0x04, 0x89, 0x4d}};
static IUIAutomation *ua;
static IUIAutomationTreeWalker *walker;
static void utf8(BSTR b, char *out, int n) {
    out[0] = 0;
    if (b != NULL) WideCharToMultiByte(CP_UTF8, 0, b, -1, out, n, NULL, NULL);
}
static int number(IUIAutomationElement *e, PROPERTYID id) {
    VARIANT v;
    VariantInit(&v);
    int r = 0;
    if (SUCCEEDED(IUIAutomationElement_GetCurrentPropertyValue(e, id, &v)) && v.vt == VT_I4) r = v.lVal;
    VariantClear(&v);
    return r;
}
static void walk(IUIAutomationElement *parent, int depth) {
    IUIAutomationElement *e = NULL;
    IUIAutomationTreeWalker_GetFirstChildElement(walker, parent, &e);
    while (e != NULL) {
        BSTR b = NULL;
        char name[128], framework[32];
        IUIAutomationElement_get_CurrentFrameworkId(e, &b);
        utf8(b, framework, sizeof framework);
        SysFreeString(b);
        if (strcmp(framework, "CeeSafe") == 0) {
            CONTROLTYPEID type = 0;
            IUIAutomationElement_get_CurrentControlType(e, &type);
            b = NULL;
            IUIAutomationElement_get_CurrentName(e, &b);
            utf8(b, name, sizeof name);
            SysFreeString(b);
            printf("  %*s%d %s", depth * 2, "", (int)type, name);
            int place = number(e, UIA_PositionInSetPropertyId), size = number(e, UIA_SizeOfSetPropertyId);
            if (place > 0) printf(" set=%d/%d", place, size);
            IUIAutomationGridPattern *grid = NULL;
            IUIAutomationElement_GetCurrentPatternAs(e, UIA_GridPatternId, &igrid, (void **)&grid);
            if (grid != NULL) {
                int rows = 0, columns = 0;
                IUIAutomationGridPattern_get_CurrentRowCount(grid, &rows);
                IUIAutomationGridPattern_get_CurrentColumnCount(grid, &columns);
                printf(" grid=%dx%d", rows, columns);
                IUIAutomationGridPattern_Release(grid);
            }
            IUIAutomationTablePattern *table = NULL;
            IUIAutomationElement_GetCurrentPatternAs(e, UIA_TablePatternId, &itable, (void **)&table);
            if (table != NULL) {
                IUIAutomationElementArray *headers = NULL;
                IUIAutomationTablePattern_GetCurrentColumnHeaders(table, &headers);
                int count = 0;
                if (headers != NULL) IUIAutomationElementArray_get_Length(headers, &count);
                printf(" headers=");
                for (int k = 0; k < count; k++) {
                    IUIAutomationElement *h = NULL;
                    IUIAutomationElementArray_GetElement(headers, k, &h);
                    b = NULL;
                    if (h != NULL) IUIAutomationElement_get_CurrentName(h, &b);
                    utf8(b, name, sizeof name);
                    SysFreeString(b);
                    printf("%s%s", k ? "," : "", name);
                    if (h != NULL) IUIAutomationElement_Release(h);
                }
                if (headers != NULL) IUIAutomationElementArray_Release(headers);
                IUIAutomationTablePattern_Release(table);
            }
            IUIAutomationGridItemPattern *cell = NULL;
            IUIAutomationElement_GetCurrentPatternAs(e, UIA_GridItemPatternId, &icell, (void **)&cell);
            if (cell != NULL) {
                int row = 0, column = 0;
                IUIAutomationGridItemPattern_get_CurrentRow(cell, &row);
                IUIAutomationGridItemPattern_get_CurrentColumn(cell, &column);
                printf(" cell=%d,%d", row, column);
                IUIAutomationGridItemPattern_Release(cell);
            }
            printf("\n");
            walk(e, depth + 1);
        }
        IUIAutomationElement *next = NULL;
        IUIAutomationTreeWalker_GetNextSiblingElement(walker, e, &next);
        IUIAutomationElement_Release(e);
        e = next;
    }
}
int main(void) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(&cuia, NULL, CLSCTX_INPROC_SERVER, &iuia, (void **)&ua))) {
        printf("no UI Automation\n");
        return 1;
    }
    HWND window = NULL;
    for (int k = 0; k < 50 && window == NULL; k++) {
        window = FindWindowW(NULL, L"access check");
        if (window == NULL) Sleep(100);
    }
    IUIAutomationElement *root = NULL;
    if (window == NULL || FAILED(IUIAutomation_ElementFromHandle(ua, window, &root)) || root == NULL) {
        printf("no window\n");
        return 1;
    }
    IUIAutomation_get_RawViewWalker(ua, &walker);
    walk(root, 0);
    fflush(stdout);
    return 0;
}
"""

# What WIN_UIA_TREE prints of the "tree" window (ADR 0207): the containers with their children, the items' places
# ("3 of 40" from the program, the rest counted), the table's grid and headers, its cells' places.
WIN_UIA_TREE_EXPECTED = [
    "  50008 Fruit",
    "    50007 Apple set=3/40", "    50007 Pear set=4/40", "    50007 Plum set=5/40",
    "  50009 Edit",
    "    50011 Cut set=1/2", "    50011 Copy set=2/2",
    "  50018 Views",
    "    50019 One set=1/2", "    50019 Two set=2/2",
    "  50028 Grid grid=2x3 headers=Name,Size,Kind",
    "    50029  set=1/2",
    "      50035 Name cell=0,0", "      50035 Size cell=0,1", "      50035 Kind cell=0,2",
    "    50029  set=2/2",
    "      50029 A cell=1,0", "      50029 1 cell=1,1", "      50029 x cell=1,2",
    "  50026 Tools",
    "    50000 Go set=1/1",
]

WIN_DROP_SOURCE = r"""/* window_check's drag source on Windows (ADR 0150): a person's drag, made with the system's mouse input (SendInput)
   and an OLE drag (DoDragDrop) of files (CF_HDROP) or of text (CF_UNICODETEXT). A press on this helper's small window
   starts the drag, the pointer moves to the target point in steps and is released there, or with `leave` back over
   the helper. It prints the effect the drag ended with (0 none, 1 copy).

   usage: dropsource drop|leave|text TITLE X Y [PATH...]
   The target is the top-level window titled TITLE; X, Y is the target point in its content, in points. */
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UINT format;
static HGLOBAL global;
static POINT start, target;
static int leave;

static HRESULT STDMETHODCALLTYPE data_query(IDataObject *self, REFIID iid, void **out) {
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDataObject)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE data_ref(IDataObject *self) {
    (void)self;
    return 1;
}
static int matches(const FORMATETC *f) {
    return f->cfFormat == format && (f->tymed & TYMED_HGLOBAL) && f->dwAspect == DVASPECT_CONTENT;
}
static HRESULT STDMETHODCALLTYPE data_get(IDataObject *self, FORMATETC *f, STGMEDIUM *medium) {
    (void)self;
    if (!matches(f)) {
        return DV_E_FORMATETC;
    }
    SIZE_T size = GlobalSize(global);
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    memcpy(GlobalLock(copy), GlobalLock(global), size);
    GlobalUnlock(global);
    GlobalUnlock(copy);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = copy;
    medium->pUnkForRelease = NULL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE data_get_here(IDataObject *self, FORMATETC *f, STGMEDIUM *medium) {
    (void)self, (void)f, (void)medium;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE data_query_get(IDataObject *self, FORMATETC *f) {
    (void)self;
    return matches(f) ? S_OK : DV_E_FORMATETC;
}
static HRESULT STDMETHODCALLTYPE data_canonical(IDataObject *self, FORMATETC *in, FORMATETC *out) {
    (void)self, (void)in;
    out->ptd = NULL;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE data_set(IDataObject *self, FORMATETC *f, STGMEDIUM *medium, BOOL release) {
    (void)self, (void)f, (void)medium, (void)release;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE data_enum(IDataObject *self, DWORD direction, IEnumFORMATETC **out) {
    (void)self;
    FORMATETC f = {(CLIPFORMAT)format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (direction != DATADIR_GET) {
        return E_NOTIMPL;
    }
    return SHCreateStdEnumFmtEtc(1, &f, out);
}
static HRESULT STDMETHODCALLTYPE data_advise(IDataObject *self, FORMATETC *f, DWORD flags, IAdviseSink *sink,
                                             DWORD *connection) {
    (void)self, (void)f, (void)flags, (void)sink, (void)connection;
    return OLE_E_ADVISENOTSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE data_unadvise(IDataObject *self, DWORD connection) {
    (void)self, (void)connection;
    return OLE_E_ADVISENOTSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE data_enum_advise(IDataObject *self, IEnumSTATDATA **out) {
    (void)self, (void)out;
    return OLE_E_ADVISENOTSUPPORTED;
}
static IDataObjectVtbl data_vtable = {data_query,     data_ref,  data_ref,  data_get,    data_get_here,
                                      data_query_get, data_canonical, data_set, data_enum, data_advise,
                                      data_unadvise,  data_enum_advise};
static IDataObject data = {&data_vtable};

static HRESULT STDMETHODCALLTYPE source_query(IDropSource *self, REFIID iid, void **out) {
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDropSource)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE source_ref(IDropSource *self) {
    (void)self;
    return 1;
}
static HRESULT STDMETHODCALLTYPE source_continue(IDropSource *self, BOOL escape, DWORD keys) {
    (void)self;
    return escape ? DRAGDROP_S_CANCEL : !(keys & MK_LBUTTON) ? DRAGDROP_S_DROP : S_OK;
}
static HRESULT STDMETHODCALLTYPE source_feedback(IDropSource *self, DWORD effect) {
    (void)self, (void)effect;
    return DRAGDROP_S_USEDEFAULTCURSORS;
}
static IDropSourceVtbl source_vtable = {source_query, source_ref, source_ref, source_continue, source_feedback};
static IDropSource source = {&source_vtable};

static void mouse(DWORD flags, POINT p) {
    INPUT input;
    memset(&input, 0, sizeof input);
    input.type = INPUT_MOUSE;
    input.mi.dx = (LONG)((double)p.x * 65535.0 / (GetSystemMetrics(SM_CXSCREEN) - 1));
    input.mi.dy = (LONG)((double)p.y * 65535.0 / (GetSystemMetrics(SM_CYSCREEN) - 1));
    input.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof input);
}

static void glide(POINT a, POINT b) {
    for (int k = 1; k <= 40; k++) {
        POINT p = {a.x + (b.x - a.x) * k / 40, a.y + (b.y - a.y) * k / 40};
        mouse(0, p);
        Sleep(15);
    }
}

static DWORD WINAPI drive(LPVOID unused) {
    (void)unused;
    Sleep(500);
    mouse(0, start);
    Sleep(300);
    mouse(MOUSEEVENTF_LEFTDOWN, start);
    Sleep(300);
    glide(start, target);
    for (int k = 0; k < 10; k++) {
        POINT p = {target.x + (k % 2), target.y};
        mouse(0, p);
        Sleep(50);
    }
    mouse(0, target);
    Sleep(300);
    if (leave) {
        glide(target, start);
        Sleep(300);
        mouse(MOUSEEVENTF_LEFTUP, start);
    } else {
        mouse(MOUSEEVENTF_LEFTUP, target);
    }
    Sleep(10000);
    printf("timeout\n");
    fflush(stdout);
    ExitProcess(2);
    return 0;
}

static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_LBUTTONDOWN) {
        DWORD effect = DROPEFFECT_NONE;
        DoDragDrop(&data, &source, DROPEFFECT_COPY, &effect);
        printf("effect %lu\n", (unsigned long)effect);
        fflush(stdout);
        ExitProcess(0);
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        HBRUSH orange = CreateSolidBrush(RGB(255, 140, 0));
        FillRect(dc, &paint.rcPaint, orange);
        DeleteObject(orange);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int wmain(int argc, wchar_t **wargv) {
    if (argc < 5) {
        fprintf(stderr, "usage: dropsource drop|leave|text TITLE X Y [PATH...]\n");
        return 2;
    }
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    FARPROC aware = GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (aware != NULL) {
        BOOL(WINAPI * set)(HANDLE);
        memcpy(&set, &aware, sizeof set);
        set((HANDLE)-4);
    }
    OleInitialize(NULL);
    leave = wcscmp(wargv[1], L"leave") == 0;
    HWND window = FindWindowW(L"CeeSafeWindow", wargv[2]);
    if (window == NULL) {
        printf("no window\n");
        return 2;
    }
    /* The target on top, away from this helper and the consoles, so the drag crosses only the desktop to it. */
    SetWindowPos(window, HWND_TOPMOST, 400, 220, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    Sleep(300);
    FARPROC dpi = GetProcAddress(user32, "GetDpiForWindow");
    double scale = 1.0;
    if (dpi != NULL) {
        UINT(WINAPI * get)(HWND);
        memcpy(&get, &dpi, sizeof get);
        scale = get(window) / 96.0;
    }
    target.x = (LONG)(_wtof(wargv[3]) * scale + 0.5);
    target.y = (LONG)(_wtof(wargv[4]) * scale + 0.5);
    ClientToScreen(window, &target);
    if (wcscmp(wargv[1], L"text") == 0) {
        const wchar_t text[] = L"window_check text";
        format = CF_UNICODETEXT;
        global = GlobalAlloc(GMEM_MOVEABLE, sizeof text);
        memcpy(GlobalLock(global), text, sizeof text);
        GlobalUnlock(global);
    } else {
        size_t units = 1;
        for (int k = 5; k < argc; k++) {
            units += wcslen(wargv[k]) + 1;
        }
        format = CF_HDROP;
        global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + units * sizeof(wchar_t));
        DROPFILES *files = (DROPFILES *)GlobalLock(global);
        files->pFiles = sizeof(DROPFILES);
        files->fWide = TRUE;
        wchar_t *list = (wchar_t *)(files + 1);
        for (int k = 5; k < argc; k++) {
            size_t n = wcslen(wargv[k]);
            memcpy(list, wargv[k], n * sizeof(wchar_t));
            list += n + 1;
        }
        GlobalUnlock(global);
    }
    WNDCLASSW c;
    memset(&c, 0, sizeof c);
    c.lpfnWndProc = procedure;
    c.hInstance = GetModuleHandleW(NULL);
    c.lpszClassName = L"WindowCheckDropSource";
    c.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    RegisterClassW(&c);
    HWND own = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, c.lpszClassName, L"", WS_POPUP | WS_VISIBLE, 60, 60,
                               120, 120, NULL, NULL, c.hInstance, NULL);
    SetForegroundWindow(own);
    start.x = 120;
    start.y = 120;
    CreateThread(NULL, 0, drive, NULL, 0, NULL);
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
"""


def real_drop(exe, work, helper):
    """ADR 0150 with a real window (--display) and a person's drags made by `helper` (macOS: the system's mouse events
    and a dragging session; Windows: SendInput and an OLE DoDragDrop; X11: an XDND source): a drag of text is refused (no drag, no drop, and the source told so);
    a drag of two files that enters, moves and leaves shows in dragOver with its count, then not; one that is dropped
    arrives with both paths, at the point where it was released, and ends the drag. A list of problems."""
    import queue
    import threading
    import time
    import unicodedata
    mac = sys.platform == "darwin"
    windows = os.name == "nt"
    files = [os.path.join(work, "ünï drop.txt"), os.path.join(work, "two.txt")]
    for path in files:
        with open(path, "w", encoding="utf-8") as f:
            f.write("window_check\n")
    env = dict(os.environ)
    env.pop("CEESAFE_WINDOW_SCRIPT", None)
    env.pop("CEESAFE_WINDOW_DUMP", None)
    p = subprocess.Popen([exe, "real"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         encoding="utf-8", errors="replace", env=env, cwd=work)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line.rstrip("\n")) for line in p.stdout], daemon=True).start()

    def take(until=None, seconds=10.0):
        """The program's lines until one that `until` accepts (or for `seconds`, without `until`)."""
        got, end = [], time.time() + seconds
        while time.time() < end:
            try:
                line = lines.get(timeout=0.1)
            except queue.Empty:
                continue
            got.append(line)
            if until is not None and until(line):
                return got
        if until is not None:
            raise RuntimeError(f"the program printed {got!r}")
        return got

    def drag(mode, target):
        command = ([helper, mode, str(p.pid), "300", "200", "150", "100"] if mac else
                   [helper, mode, "window_check drop", "150", "100"] if windows else
                   [helper, mode, target, "150", "100"]) + files
        r = subprocess.run(command, capture_output=True, text=True, timeout=60)
        return r.stdout.splitlines()

    def near(line, x, y):
        parts = line.split()
        return len(parts) >= 3 and abs(float(parts[1]) - x) <= 1.5 and abs(float(parts[2]) - y) <= 1.5

    problems = []
    try:
        ready = take(lambda line: line.startswith("ready"), 20)[-1]
        target = ready.split()[1] if len(ready.split()) > 1 else ""
        time.sleep(0.5)
        said = drag("text", target)
        refused = (["operation 0"] if mac else ["effect 0"] if windows else
                   ["aware 5", "status 0", "status 0", "status 0", "left"])
        got = take(seconds=1.5)
        if said != refused or got:
            problems.append(f"a drag of text: expected the source to see {refused!r} and the program nothing, got "
                            f"{said!r} and {got!r}")
        said = drag("leave", target)
        got = take(lambda line: line == "over none")
        if not any(line.startswith("over ") and line.endswith(" 2") and near(line, 150, 100) for line in got):
            problems.append(f"a drag that leaves: expected 'over 150 100 2' (±1.5) before 'over none', got {got!r}")
        if (mac or windows) and said != (["operation 0"] if mac else ["effect 0"]):
            problems.append(f"a drag that leaves: the source saw {said!r}")
        if not mac and not windows and (said[:2] != ["aware 5", "converted"] or "status 1" not in said or said[-1] != "left"):
            problems.append(f"a drag that leaves: the XDND source saw {said!r}")
        said = drag("drop", target)
        got = take(lambda line: line.startswith("drop "))
        got += take(seconds=1.0)
        at = got.index(next(line for line in got if line.startswith("drop ")))
        paths = [unicodedata.normalize("NFC", os.path.realpath(line.strip())) for line in got[at + 1:]]
        want = [unicodedata.normalize("NFC", os.path.realpath(path)) for path in files]
        if not near(got[at], 150, 100) or paths != want or got[at - 1:at] != ["over none"]:
            problems.append(f"a drop: expected 'over none', then 'drop 150 100' (±1.5) and {want!r}, got {got!r}")
        if not any(line.endswith(" 2") and near(line, 150, 100) for line in got[:at] if line.startswith("over ")):
            problems.append(f"a drop: expected the drag over the window before it, got {got!r}")
        finished = ["operation 1"] if mac else ["effect 1"] if windows else ["finished 1 copy"]
        if said[-1:] != finished:
            problems.append(f"a drop: expected the source to end with {finished!r}, got {said!r}")
        if p.wait(timeout=20) != 0:
            problems.append(f"a real drop: the program ended with status {p.returncode}")
    except (RuntimeError, StopIteration, ValueError, subprocess.TimeoutExpired) as e:
        problems.append(f"a real window's drops: {e!r}")
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
    return problems


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
    WindowHandle.UIKit(a, b) => if (a != 0 && b != 0) 4 else 9,
    WindowHandle.Android(a, b) => if (a != 0 && b != 0) 5 else 9,
    WindowHandle.Wayland(a, b) => if (a != 0 && b != 0) 6 else 9,
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
    if (mode == "bar") {
        // A title bar the program draws (ADR 0206): whether it got one, the layout and the actions taken, and
        // whether it is maximized (the actions only ask the system, which answers later or not at all).
        var styled = match (Window.openStyled("window_check bar", 8, 6, WindowSurface.Frames,
                                              WindowStyle.ProgramTitleBar)) {
            Ok(w) => w,
            Err(_) => { Console.writeLine("open failed"); return 1; }
        };
        let none = WindowArea { x: 0.0, y: 0.0, width: 0.0, height: 0.0 };
        styled.setTitleBar(WindowTitleBar {
            height: 3.0,
            minimize: WindowArea { x: 5.0, y: 0.0, width: 1.0, height: 3.0 },
            maximize: WindowArea { x: 6.0, y: 0.0, width: 1.0, height: 3.0 },
            close: WindowArea { x: 7.0, y: 0.0, width: 1.0, height: 3.0 },
            menu: none,
        });
        let got = styled.programTitleBar();
        Console.write("bar ");
        Console.write(got);
        Console.write(" ");
        Console.write(styled.maximized());
        Console.write(" ");
        Console.writeLine(styled.systemButtons().width > 0.0);
        return 0;
    }
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


LAST_RUN = ["nothing yet"]  # the last command run, named if the checker itself fails


def run(command, env, cwd):
    """The command's result; one that takes over 120 s is killed, and its result is status -1 with what it printed,
    so that the case that ran it reports the problem by name."""
    full = dict(os.environ)
    full.pop("CEESAFE_WINDOW_SCRIPT", None)
    full.pop("CEESAFE_WINDOW_DUMP", None)
    full.update(env)
    LAST_RUN[0] = " ".join(os.path.basename(c) if k == 0 else c for k, c in enumerate(command))
    try:
        return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", env=full,
                              cwd=cwd, timeout=120)
    except subprocess.TimeoutExpired as e:
        def text(b):
            return b.decode("utf-8", "replace") if isinstance(b, bytes) else (b or "")
        return subprocess.CompletedProcess(command, -1, text(e.stdout), text(e.stderr) + "\n(killed after 120 s)")


# Linux windows on Wayland (ADR 0205): asked for, or the default where there is a compositor.
WAYLAND = sys.platform.startswith(("linux", "freebsd")) and (
    os.environ.get("CEESAFE_WINDOW_SYSTEM") == "wayland" or
    (os.environ.get("CEESAFE_WINDOW_SYSTEM") != "x11" and
     bool(os.environ.get("WAYLAND_DISPLAY") or os.environ.get("WAYLAND_SOCKET"))))


def native_arm64():
    """Whether Windows runs on ARM64, asked of the system: an x64 Python emulated there is told AMD64 by its
    environment (PROCESSOR_ARCHITECTURE), and would take x64's libraries for an ARM64 compiler."""
    try:
        import ctypes
        process, native = ctypes.c_ushort(0), ctypes.c_ushort(0)
        kernel32 = ctypes.windll.kernel32
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p  # a pseudo-handle, 64 bits: not ctypes' default int
        current = ctypes.c_void_p(kernel32.GetCurrentProcess())
        if kernel32.IsWow64Process2(current, ctypes.byref(process), ctypes.byref(native)):
            return native.value == 0xAA64  # IMAGE_FILE_MACHINE_ARM64
    except (AttributeError, OSError):
        pass
    return os.environ.get("PROCESSOR_ARCHITECTURE", "").upper() == "ARM64"


def windows_uia_tree(access, work):
    """ADR 0207 on Windows: the access program's "tree" window, held open, as UI Automation's clients see it from
    another process (WIN_UIA_TREE). The problems found."""
    cc = os.environ.get("CC", "gcc")
    source, helper = os.path.join(work, "uiatree.c"), os.path.join(work, "uiatree.exe")
    with open(source, "w", encoding="utf-8") as f:
        f.write(WIN_UIA_TREE)
    r = subprocess.run([cc, "-o", helper, source, "-lole32", "-loleaut32", "-luser32"], capture_output=True,
                       text=True, env=msvc_environment(cc))
    if r.returncode != 0:
        return [f"a real window's containers (ADR 0207): building the UI Automation helper failed:\n{r.stderr}"]
    program = subprocess.Popen([access, "tree", "hold"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        r = subprocess.run([helper], capture_output=True, text=True, timeout=60)
    finally:
        program.kill()
        program.wait()
    got = r.stdout.splitlines()
    if r.returncode != 0 or got != WIN_UIA_TREE_EXPECTED:
        return ["a real window's containers (ADR 0207), as UI Automation reports them: expected\n" +
                "\n".join(WIN_UIA_TREE_EXPECTED) + f"\ngot (status {r.returncode})\n{r.stdout}{r.stderr}"]
    return []


def msvc_environment(cc):
    """For clang or cl on Windows outside Visual Studio's developer prompt (INCLUDE unset): the environment its
    vcvarsall.bat leaves, found as ceesafec finds it (vswhere, the C++ tools for this machine; tooling.md §8.2), so a
    helper compiled here finds windows.h and the SDK's libraries; else None (this environment as it is)."""
    if os.environ.get("INCLUDE") or "gcc" in os.path.basename(cc).lower():
        return None
    base = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = os.path.join(base, "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        return None
    arm = native_arm64()
    tools = "Microsoft.VisualStudio.Component.VC.Tools." + ("ARM64" if arm else "x86.x64")
    found = subprocess.run([vswhere, "-latest", "-products", "*", "-requires", tools, "-property",
                            "installationPath"], capture_output=True, text=True).stdout.strip()
    vcvars = os.path.join(found, "VC", "Auxiliary", "Build", "vcvarsall.bat") if found else ""
    if not vcvars or not os.path.exists(vcvars):
        return None
    shown = subprocess.run(f'"{vcvars}" {"arm64" if arm else "x64"} >nul && set', shell=True, capture_output=True,
                           text=True)
    env = dict(os.environ)
    for line in shown.stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            env[key] = value
    return env if "INCLUDE" in env else None


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
            # ADR 0201: on macOS (and iOS) the main thread is the window thread; elsewhere the first that opens one.
            ("another thread first", [errors, "thread"], {"CEESAFE_WINDOW_SCRIPT": script},
             "3" if sys.platform == "darwin" else "0"),
            ("another thread after the first", [errors, "thread-after"], {"CEESAFE_WINDOW_SCRIPT": script}, "3"),
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
            ("a scripted window asking for its own title bar (ADR 0206)", [surface, "bar"], "bar false false false", 0,
             False),
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

        drop_source, drop = build(compiler, DROP, work, "window_check_drop")
        drop_scripts = {}
        for name, content in (("drops", DROP_SCRIPT), ("dragwait", b"1:dragover:7,8:3"), ("dropwait", b"1:drop:2,3:/w"),
                              ("nocount", b"0:dragover:1,2:0"), ("noat", b"0:dragover:1,2"),
                              ("nopaths", b"0:drop:1,2"), ("leaveargs", b"0:dragleave:1,2")):
            drop_scripts[name] = os.path.join(work, name + ".txt")
            with open(drop_scripts[name], "wb") as f:
                f.write(content)
        drop_cases = [
            ("file drops", [drop], "drops", DROP_EXPECTED, 0),
            ("a drag change ends a wait", [drop, "wait"], "dragwait", DROP_WAIT_DRAG, 0),
            ("a drop ends a wait", [drop, "wait"], "dropwait", DROP_WAIT_DROP, 0),
            ("drops in the interpreter", [compiler, "run", "--interp", drop_source], "drops", ["open failed"], 1),
            ("a dragover of no files", [errors], "nocount", ["4"], 0),
            ("a dragover without a count", [errors], "noat", ["4"], 0),
            ("a drop without paths", [errors], "nopaths", ["4"], 0),
            ("a dragleave with arguments", [errors], "leaveargs", ["4"], 0),
        ]
        for what, command, name, want, status in drop_cases:
            r = run(command, {"CEESAFE_WINDOW_SCRIPT": drop_scripts[name]}, work)
            got = r.stdout.splitlines()
            if r.returncode != status or got != want:
                problems.append(f"{what}: expected\n  " + "\n  ".join(want) + f"\ngot (status {r.returncode})\n  " +
                                "\n  ".join(got) + "\n" + r.stderr)

        touch_source, touch = build(compiler, TOUCH, work, "window_check_touch")
        touch_scripts = {}
        for name, content in (("trace", TOUCH_SCRIPT), ("full", TOUCH_FULL_SCRIPT), ("wait", "1:touch:1,began,2,3"),
                              ("noplace", "0:touch:1,began,2"), ("noid", "0:touch:,began,1,2"),
                              ("lifted", "0:touch:1,lifted,1,2"), ("nophase", "0:touch:1")):
            touch_scripts[name] = os.path.join(work, "touch-" + name + ".txt")
            with open(touch_scripts[name], "w", encoding="utf-8") as f:
                f.write(content)
        touch_cases = [
            ("touches", [touch], "trace", TOUCH_EXPECTED, 0),
            ("a full queue of touches", [touch, "full"], "full", TOUCH_FULL_EXPECTED, 0),
            ("a touch ends a wait", [touch, "wait"], "wait", TOUCH_WAIT, 0),
            ("touches in the interpreter", [compiler, "run", "--interp", touch_source], "trace", ["open failed"], 1),
            ("a touch without its place", [errors], "noplace", ["4"], 0),
            ("a touch without its number", [errors], "noid", ["4"], 0),
            ("a touch of an unknown phase", [errors], "lifted", ["4"], 0),
            ("a touch without a phase", [errors], "nophase", ["4"], 0),
        ]
        for what, command, name, want, status in touch_cases:
            r = run(command, {"CEESAFE_WINDOW_SCRIPT": touch_scripts[name]}, work)
            got = r.stdout.splitlines()
            if r.returncode != status or got != want:
                problems.append(f"{what}: expected\n  " + "\n  ".join(want) + f"\ngot (status {r.returncode})\n  " +
                                "\n  ".join(got) + "\n" + r.stderr)

        safe_source, safe = build(compiler, SAFE, work, "window_check_safe")
        safe_scripts = {}
        for name, content in (("trace", SAFE_SCRIPT), ("three", "0:safe:1,2,3"), ("five", "0:safe:1,2,3,4,5"),
                              ("negative", "0:safe:1,2,3,-4")):
            safe_scripts[name] = os.path.join(work, "safe-" + name + ".txt")
            with open(safe_scripts[name], "w", encoding="utf-8") as f:
                f.write(content)
        safe_cases = [
            ("the safe area", [safe], "trace", SAFE_EXPECTED, 0),
            ("the safe area in the interpreter", [compiler, "run", "--interp", safe_source], "trace", ["open failed"], 1),
            ("a safe area of three numbers", [errors], "three", ["4"], 0),
            ("a safe area of five numbers", [errors], "five", ["4"], 0),
            ("a safe area with a negative inset", [errors], "negative", ["4"], 0),
        ]
        for what, command, name, want, status in safe_cases:
            r = run(command, {"CEESAFE_WINDOW_SCRIPT": safe_scripts[name]}, work)
            got = r.stdout.splitlines()
            if r.returncode != status or got != want:
                problems.append(f"{what}: expected\n  " + "\n  ".join(want) + f"\ngot (status {r.returncode})\n  " +
                                "\n  ".join(got) + "\n" + r.stderr)

        _, keyboard = build(compiler, KEYBOARD, work, "window_check_keyboard")
        keyboard_scripts = {}
        for name, content in (("trace", KEYBOARD_SCRIPT), ("two", "0:keyboard:1,2"), ("negative", "0:keyboard:-1")):
            keyboard_scripts[name] = os.path.join(work, "keyboard-" + name + ".txt")
            with open(keyboard_scripts[name], "w", encoding="utf-8") as f:
                f.write(content)
        keyboard_cases = [
            ("the soft keyboard", [keyboard], "trace", KEYBOARD_EXPECTED, 0),
            ("a keyboard of two numbers", [errors], "two", ["4"], 0),
            ("a negative keyboard", [errors], "negative", ["4"], 0),
        ]
        for what, command, name, want, status in keyboard_cases:
            r = run(command, {"CEESAFE_WINDOW_SCRIPT": keyboard_scripts[name]}, work)
            got = r.stdout.splitlines()
            if r.returncode != status or got != want:
                problems.append(f"{what}: expected\n  " + "\n  ".join(want) + f"\ngot (status {r.returncode})\n  " +
                                "\n  ".join(got) + "\n" + r.stderr)

        access = build_package(compiler, ACCESS, ACCESS_C, work, "window_check_access")
        access_scripts = {}
        for name, content in (("trace", ACCESS_SCRIPT), ("zero", "1:access:0,press"), ("jump", "1:access:2,jump"),
                              ("none", "1:close")):
            access_scripts[name] = os.path.join(work, "access-" + name + ".txt")
            with open(access_scripts[name], "w", encoding="utf-8") as f:
                f.write(content)
        access_cases = [
            ("a screen reader's requests", [access], "trace", ACCESS_EXPECTED, 0),
            ("a request of element 0", [errors], "zero", ["4"], 0),
            ("a request of an unknown action", [errors], "jump", ["4"], 0),
        ]
        for what, command, name, want, status in access_cases:
            r = run(command, {"CEESAFE_WINDOW_SCRIPT": access_scripts[name]}, work)
            got = r.stdout.splitlines()
            if r.returncode != status or got != want:
                problems.append(f"{what}: expected\n  " + "\n  ".join(want) + f"\ngot (status {r.returncode})\n  " +
                                "\n  ".join(got) + "\n" + r.stderr)
        for argument, message in (("twice", "an element's id is given twice"),
                                  ("outside", "an element's label or value is outside the text"),
                                  ("unknown", "an element's parent is not a container before it in the list"),
                                  ("later", "an element's parent is not a container before it in the list"),
                                  ("leaf", "an element's parent is not a container before it in the list")):
            r = run([access, argument], {"CEESAFE_WINDOW_SCRIPT": access_scripts["none"]}, work)
            if r.returncode == 0 or message not in r.stderr:
                problems.append(f"setAccessNodes, {argument}: expected a panic saying {message!r}, got status "
                                f"{r.returncode}: {r.stderr.strip()}")

        _, app = build(compiler, APP, work, "window_check_app")
        app_scripts = {}
        for name, content in (("trace", APP_SCRIPT), ("wait", "1:app:memory"), ("unknown", "0:app:sleeping"),
                              ("none", "0:app")):
            app_scripts[name] = os.path.join(work, "app-" + name + ".txt")
            with open(app_scripts[name], "w", encoding="utf-8") as f:
                f.write(content)
        app_cases = [
            ("the app's life", [app], "trace", APP_EXPECTED, 0),
            ("an app event ends a wait", [app, "wait"], "wait", ["ends waitEvent ok", "memory warning"], 0),
            ("an unknown app event", [errors], "unknown", ["4"], 0),
            ("an app event without a name", [errors], "none", ["4"], 0),
        ]
        for what, command, name, want, status in app_cases:
            r = run(command, {"CEESAFE_WINDOW_SCRIPT": app_scripts[name]}, work)
            got = r.stdout.splitlines()
            if r.returncode != status or got != want:
                problems.append(f"{what}: expected\n  " + "\n  ".join(want) + f"\ngot (status {r.returncode})\n  " +
                                "\n  ".join(got) + "\n" + r.stderr)

        _, export = build(compiler, EXPORT, work, "window_check_export")
        export_work = os.path.join(work, "export")
        os.makedirs(export_work, exist_ok=True)
        export_script = os.path.join(export_work, "script.txt")
        with open(export_script, "w", encoding="utf-8") as f:
            f.write("0:dialog:saved.txt;0:dialog;0:dialog:other.txt")
        r = run([export], {"CEESAFE_WINDOW_SCRIPT": export_script}, export_work)
        want = ["first saved", "second cancelled", "missing Resources"]

        def content(name):
            path = os.path.join(export_work, name)
            return open(path, encoding="utf-8").read() if os.path.exists(path) else None

        if (r.returncode != 0 or r.stdout.splitlines() != want or content("saved.txt") != "the draft's text" or
                content("draft.txt") != "the draft's text"):
            problems.append("exporting a file: expected\n  " + "\n  ".join(want) + " and saved.txt with the draft's "
                            f"text\ngot (status {r.returncode})\n  " + "\n  ".join(r.stdout.splitlines()) +
                            f"\nsaved.txt: {content('saved.txt')!r}\n" + r.stderr)

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
                  len(menu_cases) + len(drop_cases) + len(touch_cases) + len(safe_cases) + len(keyboard_cases) + len(app_cases) + len(access_cases) + 5 + 1 + 8)
        if args.display:
            r = run([surface, "bar"], {}, work)
            checks += 1
            # ADR 0206: the program's title bar everywhere (on X11 with a window manager that moves windows by EWMH,
            # as every desktop's does); on macOS the traffic lights stay the system's
            bar = "bar true false true" if sys.platform == "darwin" else "bar true false false"
            if r.returncode != 0 or r.stdout.strip() != bar:
                problems.append(f"a real window asking for its own title bar: expected {bar!r}, got "
                                f"{r.stdout.strip()!r} (status {r.returncode}) {r.stderr.strip()}")
            r = run([surface, "external"], {}, work)
            checks += 1
            kind = "1" if sys.platform == "darwin" else "2" if os.name == "nt" else "6" if WAYLAND else "3"
            if r.returncode != 0 or r.stdout.strip() != f"{kind} 1 1":
                problems.append(f"a real External window: expected '{kind} 1 1', got {r.stdout.strip()!r} "
                                f"(status {r.returncode}) {r.stderr.strip()}")
            r = run([text, "real"], {}, work)
            checks += 1
            if WAYLAND:
                pass  # a Wayland selection needs a serial of the user's input, which this window never has (ADR 0205 §8)
            elif r.returncode != 0 or r.stdout.strip() != "clipboard [CeeSafe ✓ clipboard]":
                problems.append(f"a real window's clipboard: expected 'clipboard [CeeSafe ✓ clipboard]', got "
                                f"{r.stdout.strip()!r} (status {r.returncode}) {r.stderr.strip()}")
            r = run([wake, "real"], {}, work)
            checks += 1
            if r.returncode != 0 or r.stdout.strip() != "real wake ok":
                problems.append(f"a real window's wake: expected 'real wake ok', got {r.stdout.strip()!r} "
                                f"(status {r.returncode}) {r.stderr.strip()}")
            r = run([touch, "real"], {}, work)
            checks += 1
            if r.returncode != 0 or r.stdout.strip() != "touches false":
                problems.append(f"a real desktop window's touches: expected 'touches false', got {r.stdout.strip()!r} "
                                f"(status {r.returncode}) {r.stderr.strip()}")
            r = run([safe, "real"], {}, work)
            checks += 1
            if r.returncode != 0 or r.stdout.strip() != "safe 0 0 0 0":
                problems.append(f"a real desktop window's safe area: expected 'safe 0 0 0 0', got "
                                f"{r.stdout.strip()!r} (status {r.returncode}) {r.stderr.strip()}")
            r = run([app, "real"], {}, work)
            checks += 1
            if r.returncode != 0 or r.stdout.strip() != "no more":
                problems.append(f"a real desktop window's app events: expected 'no more', got {r.stdout.strip()!r} "
                                f"(status {r.returncode}) {r.stderr.strip()}")
            r = run([keyboard, "real"], {}, work)
            checks += 1
            if r.returncode != 0 or r.stdout.strip() != "keyboard 0":
                problems.append(f"a real desktop window's keyboard inset: expected 'keyboard 0', got "
                                f"{r.stdout.strip()!r} (status {r.returncode}) {r.stderr.strip()}")
            if sys.platform == "darwin":
                resize = build_package(compiler, RESIZE, RESIZE_C, work, "window_check_resize")
                r = run([resize], {}, work)
                checks += 1
                if r.returncode != 0 or r.stdout.strip() != "resized in the wait":
                    problems.append("a resize inside the wait, with no event behind it: expected 'resized in the "
                                    f"wait', got {r.stdout.strip()!r} (status {r.returncode}) {r.stderr.strip()}")
                r = run([access, "real"], {}, work)
                checks += 1
                want = access_real_expected()
                if r.returncode != 0 or r.stdout.splitlines() != want:
                    problems.append("a real window's elements, as AppKit reports them: expected\n" + "\n".join(want) +
                                    f"\ngot (status {r.returncode})\n{r.stdout}{r.stderr}")
                r = run([access, "tree"], {}, work)
                checks += 1
                want = ACCESS_TREE_EXPECTED
                if r.returncode != 0 or r.stdout.splitlines() != want:
                    problems.append("a real window's containers (ADR 0207), as AppKit reports them: expected\n" +
                                    "\n".join(want) + f"\ngot (status {r.returncode})\n{r.stdout}{r.stderr}")
            if os.name == "nt":
                checks += 1
                problems.extend(windows_uia_tree(access, work))
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
            checks += 1
            mac, windows = sys.platform == "darwin", os.name == "nt"
            helper = os.path.join(work, "dragsource" if mac else "dropsource" if windows else "xdndsource")
            with open(helper + (".m" if mac else ".c"), "w", encoding="utf-8") as f:
                f.write(MAC_DRAG_SOURCE if mac else WIN_DROP_SOURCE if windows else XDND_SOURCE)
            cc = os.environ.get("CC", "gcc" if windows else "cc")
            command = ([cc, "-fobjc-arc", "-o", helper, helper + ".m", "-framework", "AppKit"] if mac else
                       [cc, "-municode", "-o", helper + ".exe", helper + ".c", "-lole32", "-lshell32", "-luuid",
                        "-lgdi32", "-luser32"] if windows else [cc, "-o", helper, helper + ".c", "-lX11"])
            r = subprocess.run(command, capture_output=True, text=True,
                               env=msvc_environment(cc) if windows else None)
            if r.returncode != 0:
                problems.append(f"a real window's drops: building the drag source failed:\n{r.stderr}")
            elif not WAYLAND:  # drops on Wayland are ADR 0205's step 2; this source speaks X11's XDND
                problems.extend(real_drop(drop, work, helper + (".exe" if windows else "")))
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
    try:
        main()
    except Exception:  # a fault of the checker's own: say where it was, then the traceback
        print(f"window_check: the checker failed after running {LAST_RUN[0]!r}", file=sys.stderr)
        raise
