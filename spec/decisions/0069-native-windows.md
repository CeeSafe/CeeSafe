# ADR 0069 — Native windows in lib/std

- Status: **Accepted** by the owner, 2026-11-04. The owner asked for a native app demo of the ClayWind port, chose
  "Add windowing to CeeSafe first", then accepted §5 as proposed: all three backends, `nextEvent`, the
  `CEESAFE_WINDOW_SCRIPT` switch, premultiplied RGBA8 `u32`s, and release as 0.13.0 ("Accept, implement it").
- Scope:
  - `lib/std/window.cee` (new) and `runtime/hosted/ceesafe_rt_window.c` (new);
  - the interpreter's model of the new runtime calls (`src/interpreter/io.cee`);
  - the embedded library and runtime (`tools/gen_library.py`, `tools/gen_runtime.py`);
  - spec: type-system.md (a new §26 subsection), `spec/api/std.md` (generated), guide.

## 1. Context

- **There is no way to open a window.** lib/std reaches the operating system through `extern "C"` calls into the C
  runtime (`lib/std/host.cee` → `runtime/hosted/ceesafe_rt_io.c`): files, sockets, processes, clocks, threads. None
  of those is a window, so a CeeSafe program that wants one needs `extern "C"` in its own code.
- **The ClayWind port shows the cost.** It is a pure-CeeSafe UI toolkit: a layout engine, a software rasterizer and
  controls, with a rule of no `extern "C"` in the port. It can show its frames only headless, in a terminal, or in a
  browser fed over a WebSocket.
- **What a window has to provide is small.** The rasterizer already produces RGBA pixels. A window needs to:
  - show them;
  - report the pointer, the keys, typed text, the wheel, a resize and a close request;
  - set the cursor.

## 2. Proposal

### 2.1 The API (`lib/std/window.cee`, hosted only)

```ceesafe
let window = Window.open("ClayWind", 1024, 640)?;      // Result<Window, WindowError>
loop {
    while (true) {
        match (window.nextEvent()) {                     // WindowEvent?, never blocks
            Some(WindowEvent.Closed) => { return Ok(); }
            Some(e) => { handle(e); }
            null => { break; }
        }
    }
    window.present(pixels, width, height)?;              // premultiplied RGBA8, rows top to bottom
    window.waitFrame();                                  // until the display's next refresh, or an event
}
```

- **`Window`** is a move type with a drop, which closes the window. It is neither `Send` nor `Share`: macOS lets
  only the main thread use windows, so `Window.open` on another thread is `WindowError.NotMainThread` on every
  system, which keeps programs portable.
- **Opening and closing:**
  - `Window.open(title, width, height)` takes a size in points.
  - `WindowError` is `Unsupported` (no backend, including under the interpreter), `NoDisplay` (no window server,
    such as `DISPLAY` unset or a CI runner), `NotMainThread` or `Resources`.
- **Size:** `window.size()` gives `WindowSize { width, height }` in points, and `window.scale()` gives pixels per
  point (2.0 on a Retina display).
- **Drawing:** `window.present(pixels: Slice<u32>, width, height)` shows the image scaled to the window. Each
  `u32` is one premultiplied RGBA8 pixel, bytes R, G, B, A in memory order: the format of ClayWind's `Raster`
  frame, so it is passed without a copy. A program that wants sharp pixels renders at `size() * scale()`.
- **Window settings:** `window.setCursor(WindowCursor)` takes `Default`, `Pointer`, `Text`, `Crosshair`, `Grab`,
  `ResizeHorizontal` or `ResizeVertical`. `window.setTitle(StringView)` sets the title.
- **Events:** `WindowEvent` is an enum, positioned in points:
  - `Closed`, `Resized(WindowSize)` and `Focus(bool)`;
  - `PointerMoved(x, y)`, `PointerButton(button, down, x, y)` and `Wheel(dx, dy)`;
  - `Key(WindowKey, down, repeat, WindowModifiers)` and `Text(u32)`, a Unicode scalar.
- **Keys:** `WindowKey` names the keys a UI needs: letters, digits, arrows, Tab, Enter, Escape, Backspace, Delete,
  Home, End, PageUp, PageDown, Space, F1–F12, and `Other(u32)` for the rest.
- **`waitFrame()`** sleeps until about the next 1/60 s, or until an event arrives, so an idle program does not
  spin.

### 2.2 The runtime (`runtime/hosted/ceesafe_rt_window.c`)

- **The ABI.** A handful of C functions, in the style of `ceesafe_rt_io.c`: `ceesafe_window_open`, `_close`, `_poll`
  (one event into a plain struct), `_present`, `_size`, `_scale`, `_set_cursor`, `_set_title`, `_wait`. Each call
  returns a value or a negative error code.
- **macOS.**
  - **Loading.** AppKit, CoreGraphics, QuartzCore and the Objective-C runtime are opened with `dlopen` the first
    time a window is opened, and called through `objc_msgSend`. So no program needs a link flag (the runtime has no
    way to name a framework, ADR 0047), and a program that never opens a window does not load AppKit.
  - **Events and drawing.** Events come from a manual pump (`nextEventMatchingMask:untilDate:inMode:dequeue:`), as
    SDL and GLFW do, so the CeeSafe program keeps its own loop. A frame is shown by setting a layer-backed view's
    `contents` to a `CGImage` of the pixels.
- **Windows.** Win32: `RegisterClassW`, `CreateWindowExW`, `PeekMessageW`, and `StretchDIBits` for a frame. user32
  and gdi32 are named with `#pragma comment(lib)`, as `ceesafe_rt_io.c` names ws2_32.
- **Linux.** X11: `libX11.so.6` is opened with `dlopen`, and frames are shown with `XPutImage`. Without the library
  or a `DISPLAY`, `open` is `NoDisplay`. Wayland is not in this proposal (§5, question 1).
- **Elsewhere.** On any other system, and in the freestanding profile, `open` is `Unsupported`, and the runtime file
  holds only that stub.

### 2.3 Testing

- **The release workflow cannot look at a window.** Its runners have no display, or none a test can see.
- **Proposed.** A runtime switch, `CEESAFE_WINDOW_SCRIPT=<file>`, replaces the real system with a scripted one:
  - the window exists only in memory;
  - events come from the file, in the grammar of ClayWind's `ScriptPlatform`;
  - `present` keeps the last frame, and `CEESAFE_WINDOW_DUMP=<file>` writes it out.

  A new contract, `tools/window_check.py`, runs a corpus program through that, on every system, and compares the
  frame and the event trace with expectations computed in Python.
- **On macOS,** an opt-in check (`--display`) also opens a real window, presents frames, resizes it and closes it.
  It is for a machine with a display, not CI.

### 2.4 ClayWind (after a release)

- **A new host, `claywind-hosts/desktop`.** It is the demo in a native window, 100% `.cee`: it implements ClayWind's
  `Platform` on `WindowEvent`s, and presents the `Raster`'s pixels at the window's pixel size.
- **Packaging.** A `Clay.app` bundle script for macOS.

## 3. Alternatives considered

- **A browser window** (`--app` mode over the existing browser host). It works today, but it is not native: the
  pixels go through a WebSocket.
- **`extern "C"` in the app.** It breaks the port's rule, and every CeeSafe program would repeat the work.
- **Wrapping SDL.** It is a C dependency that each user would have to install, which goes against the runtime's
  self-contained design.

## 4. Risks

- **macOS through `objc_msgSend`.** The ABI depends on the call's types: arguments and results that are structures
  (`NSRect`, `CGSize`) need correctly cast calls, and on x86_64, `objc_msgSend_stret` for some of them. The two
  macOS release targets, arm64 and x86_64, both need the check of §2.3.
- **Size.** About 1,500 lines of C in three backends, plus the library and the interpreter's model. The release
  then grows its runtime, though not its binaries' dependencies.

## 5. Questions for the owner

1. **Backends:** all three now (macOS, Windows, Linux X11), or macOS first with the others `Unsupported`?
   Proposed: all three, since the release builds six targets and an API that works on one of them invites
   non-portable programs. Wayland stays later.
2. **Events:** `nextEvent() -> WindowEvent?` (no allocation, proposed), or `poll(into: borrow mut
   Vector<WindowEvent>)`?
3. **Pixels:** premultiplied RGBA8 in `u32`s only (proposed; ClayWind's `Raster` frame as it is), or also
   straight alpha and BGRA?
4. **Testing:** the `CEESAFE_WINDOW_SCRIPT` switch of §2.3 (proposed), or a separate test-only API?
5. **Release:** 0.13.0, a minor version for a new library API (proposed).


## 6. Consequences

- **The new names are taken.** `Window`, `WindowEvent`, `WindowKey`, `WindowModifiers`, `WindowButton`,
  `WindowCursor`, `WindowSize` and `WindowError` become lib/std names, which a program may not redefine (N0201). The
  repository's only clash was the corpus test `run-pass/region-near-misses.cee`, whose `struct Window<region r>` is
  renamed `Pane`, with a note.
- **The spec** is type-system.md §26.13.

## 7. Implementation and evidence (2026-11-04)

**Files:**
- `lib/std/window.cee`: the API of §2.1, decoding the runtime's events.
- `runtime/hosted/ceesafe_rt_window.c`: the scripted window and three backends, embedded as runtime file 8. The
  backend adds it only to a program whose C calls `ceesafe_window_` (`src/backend/c_backend.cee`).
- `src/interpreter/io.cee`: `ceesafe_window_open` answers `Unsupported`.
- type-system.md §26.13 and `spec/api/std.md`.

**Implementation notes:**
- **macOS colour.** The macOS backend tags frames as sRGB, so they are colour-managed on wide-gamut displays like
  the browser's canvas. Tagged as DeviceRGB, the same values looked oversaturated.
- **Windows.** The Win32 backend uses wide resource names (`MAKEINTRESOURCEW`), since the runtime is compiled
  without `UNICODE`. It records the first thread with a `.CRT$XCU` initializer, or a constructor outside MSVC.

**Tests:**
- **`tools/window_check.py`,** a new contract, makes 7 checks:
  - a scripted trace of every event kind: across frames, a resize and a focus that change nothing, and non-ASCII
    text. It and the last frame's bytes must equal what the script computes by §26.13's grammar;
  - a malformed and a missing script, a zero size, another thread, a plain scripted open, and the interpreter.

  `--display` adds a real window.
- **`check-fail/window-not-send`:** a `Window` moved into `Thread.spawn` is N0334.
- **The release workflow's smoke test** builds and runs a scripted window on all six targets.

**Real windows**, each driven by injected input, with the frames checked in screenshots:
- **macOS** (arm64, clang): the frames, the scale of 2, and the colours after sRGB tagging.
- **Windows 11** (arm64, a Parallels VM):
  - built with MSVC `/std:c11 /W3 /O2`, with no warnings;
  - mouse, keys, text and a resize, through `SendInput`, `SendKeys` and `MoveWindow`;
  - the x64 build compiles and runs too.
- **Ubuntu 22.04** (arm64, gcc under the strict warnings, Xvfb): mouse, keys, text and a resize, through `xdotool`.
  `WM_DELETE_WINDOW` gives `Closed`.
- **The same scripted run** gives byte-identical output and frames on macOS and Windows.

**Downstream: ClayWind's `claywind-hosts/desktop`,** the demo in a native window, runs on all three systems. The
owner ran it on the Mac and on the Windows VM: "Worked perfectly on both mac and windows vm".

**Corpus:**
- The full corpus passes.
- `check-pass/async-timeout.hir` and `effects-of.hir` were regenerated, since lib/std's new items shift closure
  numbers. Only numbers differ, as when ADRs 0055, 0057 and 0060 grew lib/std.

**Found on the way:** a library file's type errors are reported only by `check --library-diagnostics`, the
`library` contract. A user program that uses the file gets misleading results instead, such as N0212 or an
"unreachable" panic. Anyone changing lib/std should run that contract first.

## 8. After the release: an sRGB window on macOS (0.13.1)

- **What profiling found.** ClayWind's desktop host on a Retina display, profiled after it gained incremental
  rendering, spent most of its time presenting frames. Core Animation converted every new sRGB `CGImage` to the
  display's colour space on the CPU: vImage table lookups, about 500 samples against about 230 for all of the
  rendering.
- **The fix.** The window's colour space is now sRGB (`setColorSpace:` with `NSColorSpace.sRGBColorSpace`). Its
  backing store then matches the frames, and the window server matches the window to the display.
- **Result:** the host's CPU use while idle went from 47% to 31%, and vImage left the profile. A screenshot's
  colours are within 1 of the earlier ones.
- **No API change.** The other backends are unchanged.

