# ADR 0070 — Presenting only the changed parts of a window's frame

- Status: **Accepted** by the owner, 2026-11-04 ("Add a present that takes the changed boxes", then §5 as proposed:
  `presentChanged`, an IOSurface on macOS, a whole-frame present for a box outside the image, and release as 0.14.0).
  Raised by ClayWind's desktop host (ClayWind-PORT-PLAN.md §19).
- Scope: `lib/std/window.cee`, `runtime/hosted/ceesafe_rt_window.c` (all backends), the interpreter's model, type-system.md §26.13,
  `tools/window_check.py`.

## 1. Context

- **What ClayWind's desktop host costs now.** It redraws only what changed (ClayWind's `Damage`), and a typical Retina
  frame takes 2–4 ms to draw. Its idle CPU is still 31%, because `Window.present` takes the whole frame every time.
- **Why every frame changes.** The demo's rotating badge changes every frame. Each present then copies 10 MB
  (2048 × 1280 × 4) into a new `CGImage`, and Core Animation uploads all of it.
- **The cause.** The caller knows which pixels changed, but `present` has no way to say so.

## 2. Proposal

```ceesafe
public struct WindowBox { public x: u32; public y: u32; public width: u32; public height: u32; }

/// Like `present`, but only the pixels inside `changed` differ from the last frame presented.
public fn presentChanged(borrow mut this, pixels: Slice<u32>, width: u32, height: u32,
                         changed: Slice<WindowBox>) -> Result<void, WindowError>
```

### 2.1 The contract

- **What the caller promises.** Outside the boxes, every pixel equals the last frame presented. The boxes are in the
  image's pixels and may overlap.
- **When it is a plain `present`.** The window shows everything when there is no previous frame, when the image's
  size differs from the last one, or when a box is not inside the image (§5, question 3).
- **Empty `changed`.** It shows nothing new.
- **Pixels outside the boxes.** The runtime may show them from `pixels` or from its own copy of the last frame. A
  program that breaks the promise sees either.
- **Checks.** The same as `present`'s: `pixels.length ≥ width × height`, and `Resources` otherwise.

### 2.2 The backends

- **macOS.**
  - **The frame's storage.** The layer's contents become an IOSurface (IOSurface.framework, loaded with dlopen like
    the rest). It is tagged sRGB, with 32-bit BGRA pixels.
  - **A partial present** writes only the boxes into the surface, converting RGBA to BGRA as it goes, and tells the
    layer the contents changed. Core Animation then shares the surface with the window server, with no `CGImage`
    and no copy of the frame.
  - **A full present** writes the whole surface. A new size makes a new surface.
- **Windows.** The runtime keeps the frame as BGRA for `WM_PAINT`.
  - **At 1:1** (the image's size equals the client area's): only the boxes are converted, then drawn with
    `SetDIBitsToDevice`.
  - **Otherwise:** the boxes are converted, and the whole frame is drawn with `StretchDIBits`.
- **Linux (X11).** The runtime keeps the frame.
  - **At 1:1** (the image's size equals the window's): only the boxes are converted, then drawn with `XPutImage`.
  - **Otherwise:** the whole frame is scaled, as now.
- **Scripted windows** keep a copy of the frame and apply the boxes to it, so `CEESAFE_WINDOW_DUMP` is the frame
  the window would show. A test of a program's boxes is then a comparison of dumps.

### 2.3 ClayWind (after a release)

- **`Damage`** returns the boxes its last `render` redrew, in pixels. A full redraw is one box over the whole frame.
- **The desktop host** passes them to `presentChanged`, and presents nothing when nothing changed.

## 3. Alternatives considered

- **`present` with an optional box argument.** It is one call fewer to learn. But `present`'s promise is "this is
  the frame"; this call promises something about pixels it is not given, so it should be named apart.
- **The window works out what changed itself,** by comparing the frame with its copy. That needs no API, but it
  reads all 10 MB of the frame every time, which is the cost this proposal removes.
- **On macOS, a persistent `CGImage` over a shared buffer.** Core Animation treats a `CGImage` as immutable, and
  would still upload all of it.

## 4. Testing

- **`tools/window_check.py`, scripted:**
  - a full frame, then frames that change only some boxes, then frames that change nothing;
  - then a frame with a new size, and a frame whose box is outside the image.

  Each dump must equal the frame a plain `present` gives, which the script computes.
- **`--display`:** `presentChanged` on a real window, at 1:1 and scaled.
- **By hand:** on macOS, Windows (the Parallels VM) and Linux (Xvfb), a moving box drawn by `presentChanged`
  alone, checked in screenshots.
- **ClayWind's desktop host on a Retina display:** idle CPU and a profile, before and after.

## 5. Questions for the owner

1. **Name:** `presentChanged` (proposed), `presentRegions`, or `presentDamaged`?
2. **macOS:** an IOSurface (proposed), or a smaller change that still makes a `CGImage` per frame but converts and
   copies only the boxes? The second avoids IOSurface but keeps the full upload.
3. **A box outside the image:** present the whole frame (proposed, forgiving), or `WindowError.Resources`?
4. **Release:** 0.14.0 (proposed), a minor version for a new library API.

## 6. Implementation and evidence (2026-11-04)

**Files:**
- `lib/std/window.cee`: `WindowBox` and `presentChanged`. It passes up to 64 boxes to the runtime as four `u32`s each;
  more are a plain `present`.
- `runtime/hosted/ceesafe_rt_window.c`: `ceesafe_window_present_changed`.
  - **Every backend:** the shared code remembers the last present's size, skips empty boxes, and falls back to
    `present` when there is no frame of this size or a box is outside the image.
  - **macOS:** two sRGB BGRA IOSurfaces are shown in turn. Each keeps the boxes it still lacks (`pending`, up to 16,
    else all of it), so a surface is brought up to date only by what changed since it was last shown. Without
    IOSurface.framework, frames are `CGImage`s as before.
  - **Windows:** `StretchDIBits` of each box at 1:1. Its source rectangle's y counts from the bottom row, even for a
    top-down DIB, as testing on Windows 11 found.
  - **X11:** `XPutImage` of each box at 1:1.
  - **Scaled windows,** on Windows and X11, draw the whole frame.
  - **Scripted windows** keep the frame and apply the boxes to it.

**Tests:**
- **`tools/window_check.py` now has 15 checks.** The new ones run 1 to 8 steps of a program that paints the whole
  image a new colour each step but declares only some boxes. Each last frame must equal what the script computes:
  - no previous frame, a new size, or a box outside the image show the whole frame;
  - an empty list shows nothing new;
  - a zero-size box is skipped.
- **A real window on each system:** a grid presented once, then a square moving 4 pixels a frame through
  `presentChanged` of its old and new places only. Each screenshot shows the whole grid and exactly one square, with
  no trail:
  - **macOS** (Retina, through IOSurface): 6400 red pixels, one 80-pixel square;
  - **Windows 11 ARM64** (MSVC): 1600 red pixels at 1:1; scaled after a resize, one square;
  - **Ubuntu on Xvfb** (gcc, strict warnings): 1600 red pixels at 1:1, and 3600 scaled to 600 × 450.

