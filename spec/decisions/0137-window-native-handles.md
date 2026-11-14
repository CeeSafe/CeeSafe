# ADR 0137 — A window's native handles, and windows a program draws itself

- Status: **Accepted** by the owner, 2026-11-14. ClayWind's roadmap needs a GPU renderer. The owner chose, between "a
  GPU API in lib/std" and "`Window` exposes its native handles and ClayWind keeps a small `extern "C"` GPU layer",
  the second ("I choose the faster way"), then asked for this change ("go ahead with I1": ClayWind's
  `CEESAFE-FINDINGS.md` §I, gap I1). Implemented on branch `adr/window-native-handles`.
- Scope: `lib/std/window.cee`, `runtime/hosted/ceesafe_rt_window.c`, the interpreter's model of
  `ceesafe_window_open` (`src/interpreter/io.cee`), type-system.md §26.13, `spec/api/std.md` (generated),
  `tools/window_check.py`. No language or emitter change.

## 1. Context

- **A window shows only CPU frames.** `Window.present` and `presentChanged` (ADR 0069, ADR 0070) take premultiplied
  RGBA8 pixels from the CPU. A program that draws with the GPU (Metal, Vulkan, Direct3D 12) needs the system's own
  window to make a surface on, and `Window` hides it.
- **The GPU API is the program's.** A GPU API in lib/std would be a large surface for CeeSafe to own: three systems'
  APIs, shaders, and their versions. ClayWind instead keeps its GPU code in its own `extern "C"` layer and needs from
  lib/std only:
  - the system's handles for the window it opened;
  - a window that the runtime does not draw into, so that its frames and the GPU's do not fight over the content;
  - an event when the content's pixel size changes, so that a swap chain is resized before the next frame.
- **The scale changes silently.** Each backend updates the scale when the window moves to a display with another
  scale (`backingScaleFactor` on macOS, `WM_DPICHANGED` on Windows), but no event says so. A program that sizes its
  frames at `size() × scale()` learns of it only by asking every frame.

## 2. Decision

### 2.1 The API (lib/std)

```ceesafe
var window = Window.openWith("ClayWind", 1024, 640, WindowSurface.External)?;
match (window.nativeHandle()) {
    WindowHandle.AppKit(nswindow, nsview) => { /* a CAMetalLayer on the view, in extern "C" code */ }
    WindowHandle.Win32(hwnd, hinstance) => { /* a DXGI swap chain, or a Vulkan surface */ }
    WindowHandle.Xlib(display, xwindow) => { /* vkCreateXlibSurfaceKHR */ }
    WindowHandle.Scripted => { /* CEESAFE_WINDOW_SCRIPT: no system window */ }
}
```

- **`WindowSurface`** says who draws the content:
  - `Frames`: the runtime shows the frames given to `present` and `presentChanged`. `Window.open` is
    `openWith(…, WindowSurface.Frames)`.
  - `External`: the program draws the content itself, through `nativeHandle()`. The runtime never paints it. `present`
    and `presentChanged` are `WindowError.Unsupported`.
- **`window.nativeHandle()`** is a `WindowHandle`:
  - `AppKit(window, view)`: the `NSWindow` and its content `NSView`, which is layer-backed;
  - `Win32(window, instance)`: the `HWND` and the `HINSTANCE` that registered its class;
  - `Xlib(display, window)`: the `Display *` and the `Window` XID;
  - `Scripted`: a scripted window, which has no system window.

  The handles are `u64`s, not pointers. Safe code cannot dereference an integer, so only `unsafe` code that casts it
  back can use one. A handle is valid until the `Window` is dropped, and like the `Window`, it is used on the
  program's first thread. A program may draw through it and may add layers or child surfaces. It must not close,
  release or re-parent the window, or replace its delegate (macOS) or window procedure (Windows): the runtime's
  events depend on them.
- **`Resized` on a scale change.** When the scale changes, `nextEvent` reports `Resized` with the content's size,
  even if the size in points did not change. Then `size() × scale()` is the new pixel size. No `WindowEvent` variant
  is added, so every existing `match` keeps compiling.
- **Errors stay as they are.** `present` on an external window is `Unsupported`, an existing `WindowError`, so no
  `match` over `WindowError` breaks either.

### 2.2 The runtime

- **`ceesafe_window_open`** takes the surface, `0` for frames or `1` for external, after the size. Any other number
  is `Unsupported`.
- **`ceesafe_window_native(window, first, second) -> i32`** writes the two handles and returns the kind: `0` scripted,
  `1` AppKit, `2` Win32, `3` Xlib.
- **External windows,** per backend:
  - **macOS:** the content view stays layer-backed. The runtime never sets the layer's `contents`. A program may set a
    `CAMetalLayer` as the view's layer, or add one as a sublayer.
  - **Windows:** `WM_PAINT` validates the content without painting it, and `WM_ERASEBKGND` already does nothing. A
    DXGI flip-model swap chain or a Vulkan surface then owns the pixels.
  - **Linux:** the window's background is `None` (`XSetWindowBackgroundPixmap`), so the X server does not clear what
    a Vulkan surface drew when part of the window is exposed. `Expose` draws nothing.
  - **Scripted:** `present` is `Unsupported`, and no frame is dumped.
- **The scale.** Each backend reports a changed scale through one helper. It stores the scale and queues `Resized`
  with the current size:
  - macOS: `backingScaleFactor`, read after each pump;
  - Windows: `WM_DPICHANGED`, and the DPI read after each pump;
  - Linux: X11 reports no scale, which stays 1.
- **A scripted scale.** The script grammar gains `FRAME:scale:S`, with S above 0 and at most 16. A scale that changes
  the window's gives `Resized`, so the contract checks it on every system.

### 2.3 The interpreter

`ceesafe_window_open` keeps answering `Unsupported`, now with the error at its sixth argument.

## 3. Alternatives considered

- **A GPU API in lib/std.** It keeps programs free of `extern "C"`, but CeeSafe would own Metal, Vulkan and
  Direct3D 12 bindings, their shader formats and their versions. The owner chose the smaller change.
- **Raw pointers (`*mut void`) as handles.** They say what the values are. But a public lib/std function would return
  raw pointers to safe code, which no lib/std API does today. An integer carries the same bits, and `unsafe` code
  converts it back.
- **A `WindowEvent.ScaleChanged(f32)` variant.** It is more explicit, but it breaks every exhaustive `match` over
  `WindowEvent`, ClayWind's among them. A program needs the new pixel size, which `Resized` already prompts it to
  compute.
- **A new `WindowError.ExternalSurface`.** It names the mistake, but it breaks exhaustive matches over `WindowError`.
  `Unsupported` fits: an external window does not support presenting.

## 4. Consequences

- **New names:** `WindowSurface` and `WindowHandle` become lib/std names, which a program may not redefine (N0201).
- **Programs that draw with the GPU need `extern "C"`** for the GPU calls themselves. lib/std gives them the window,
  the events and the run loop.
- **Out of scope:** live resizing on macOS. During a drag of the window's edge, AppKit runs its own event loop inside
  `sendEvent:`, so the program's loop, and with it its frames, waits until the drag ends. Both kinds of window have
  this today, and a fix needs the runtime to call the program back, a larger change.
- **The spec** is type-system.md §26.13.

## 5. Implementation and evidence (2026-11-14)

**Files:**
- `lib/std/window.cee`: `WindowSurface`, `WindowHandle`, `Window.openWith`, `Window.nativeHandle`; `open` is
  `openWith(…, Frames)`.
- `runtime/hosted/ceesafe_rt_window.c`: the surface argument, `ceesafe_window_native`, `cee_window_rescaled` (used by
  the macOS and Windows backends and the script's `scale` item), and the external window on each backend (§2.2).
- `src/interpreter/io.cee`: `ceesafe_window_open`'s signature, `PsuuuP:P`, and the error at its sixth argument.
- type-system.md §26.13 and `spec/api/std.md`.

**Tests**, macOS arm64, Apple clang 21:
- **`tools/window_check.py`,** 15 checks before, 18 now, 20 with `--display`, all passing:
  - the scripted trace gains `scale:1` (no change, no event) and `scale:2` twice (one `Resized` at the same size),
    and ends with `scale 2`;
  - a scripted `Frames` window presents (handle `Scripted`, `0 0 0`); an `External` one answers `Unsupported` to
    `present` and `presentChanged` and dumps no frame (`0 1 1`); the interpreter answers `Unsupported` to `openWith`;
  - with `--display`, a real `External` window gives `AppKit` with non-zero handles.

  The baseline compiler (0.45.1) fails the new checks: it has no `WindowHandle` (N0204).
- **A Metal spike** (a throwaway, not committed): a CeeSafe program opened an `External` window of 480 × 300 points,
  and 90 lines of C set a `CAMetalLayer` on the `NSView` that `nativeHandle()` gave and cleared it every frame. 180
  frames, none failed, a 960 × 600 drawable at scale 2, and a screenshot of the window showed the clear colour.
- **The runtime file** compiles without warnings under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` on macOS
  and under gcc 12 (`-Wall -Wextra -Wpedantic -Wshadow`, Debian 12 arm64, in a container). The Win32 backend was
  not compiled: no Windows headers on this machine. Its change is three small edits (§2.2), and the release
  workflow's Windows build and smoke test compile and run it.
- **The corpus,** all families: every test passes after one reviewed change. Five `check-pass` HIR snapshots
  (`async-timeout`, `copied-change-warning`, `effects-of`, `string-braces-warning`, `unused-result-warning`) differ
  from before only in closure numbers, each 3 higher, since lib/std has three more closures. A script that replaced
  every closure number found the old and new dumps otherwise equal, and the snapshots were updated, as for ADR 0069.
- **`check --library-diagnostics`:** no diagnostics.

**Not run locally,** with the owner's leave to skip tests for the release: the full contract suite, the fixed point
and the seed's bootstrap. The release workflow's seed job runs the fixed point and the bootstrap, and publishes
nothing if they fail.
