# ADR 0201 — On Linux, FreeBSD and Windows, the window thread is the first that opens a window

- Status: **Accepted** by the owner, 2026-11-21 ("I accept ADRs 0201 and 0202"); implemented (§5). Found by
  ClayWind's Kotlin binding on the owner's Fedora 45 PC.
- Scope: the hosted window runtime's first-thread test on Linux, FreeBSD and Windows (`cee_window_on_main_thread`),
  type-system.md §36, a window check. macOS, iOS and Android keep their rule. No language or library change.

## 1. Context

Windows, their events and their waits belong to one thread. On every system the runtime takes it to be the process's
first thread: `Window.open` on another is `NotMainThread`. On macOS and iOS the system requires it (AppKit and UIKit
run on the main thread); on Android the runtime records the thread it starts `main` on. On Linux (X11 through `dlopen`),
FreeBSD and Windows nothing in the system does: X11 serves a display connection on any thread, and a Win32 window
belongs to the thread that made it.

A host program does not always own the first thread. A JVM runs Java's `main` on a thread it starts; macOS has
`java -XstartOnFirstThread` for AppKit, Linux and Windows have nothing like it. ClayWind's Kotlin binding, which runs
on macOS, stops there with `NoWindow` on Fedora 45 (2026-11-21), where its C and Rust bindings open their windows. The
same holds for any host whose runtime keeps the first thread for itself (a Python interpreter's own thread, a game
engine's).

## 2. Decision

1. On Linux, FreeBSD and Windows, **the window thread is the first thread that opens a window**, recorded once by
   that open (an atomic compare-and-set, so two first opens at once give one winner). Every later window call is on it:
   another thread's `open` is `NotMainThread`, as before.
2. A program that opens its windows on its first thread, which every CeeSafe program with a `main` does, sees no
   change. The Windows C runtime initializer that records the first thread goes: the first open records it.
3. macOS and iOS keep the main thread (the system's rule); Android keeps the thread its activity glue starts `main` on.
4. type-system.md §36 says "the window thread" where it said "the program's first thread", with the rule per system.

## 3. Consequences

- ClayWind's Kotlin binding runs on Linux and on Windows as it does on macOS, and so does any host that calls
  CeeSafe from a thread other than its first.
- `NotMainThread` still guards against a second thread, which is what the rule was for: the runtime keeps one
  thread's state for windows (their list, the wake, the display connection).
- A program that opened its first window on a worker thread by mistake, and later on its first thread, now fails on the
  second open instead of the first. Programs written to the old rule never do that.

## 4. Implementation

- `runtime/hosted/ceesafe_rt_window.c`: on Linux, FreeBSD and Windows, `cee_window_on_main_thread` compares the
  calling thread with the recorded window thread, recording it when there is none (`cee_window_claim_thread`, called by
  `open` only); the other calls compare only.
- type-system.md §36; a window check: a program whose window is opened on a thread it starts (Linux and Windows), and a
  second thread's open refused.
- ClayWind: the Kotlin binding's example on the Fedora PC.

## 5. As built

- `ceesafe_rt_window.c`: on Linux, FreeBSD and Windows, `cee_window_thread_id` is recorded by the first `open` with a
  compare-and-set (`__atomic` builtins; `InterlockedCompareExchange` on Windows), and `cee_window_on_main_thread`
  compares with it; Windows' C runtime initializer that recorded the first thread is gone. macOS, iOS, Android and
  other systems keep their test.
- type-system.md §36 (`NotMainThread`). window_check: a window opened first from a spawned thread is refused on macOS
  and opens elsewhere; once a window is open, another thread's open is refused on every system (64 checks on macOS
  and on Fedora 45; on the owner's Windows 11 ARM64 VM too, the compiler built there with LLVM's clang).
- ClayWind's Kotlin binding on the owner's Fedora 45 PC: its window opens from the JVM's main thread, beside the C and
  Rust ones.
