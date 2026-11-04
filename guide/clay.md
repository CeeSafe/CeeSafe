# Clay: a UI toolkit in CeeSafe

[Clay](https://github.com/CeeSafe/Clay) is ClayWind ported to CeeSafe, with no C code and no dependencies.
ClayWind is a Tailwind-flavoured immediate-mode UI toolkit built on the layout model of
[Clay by Nic Barker](https://github.com/nicbarker/clay). The port is about 18,000 lines. It is a working example of
what CeeSafe is for: a program that does everything itself, from layout to pixels, and still runs on every desktop.

![The ClayWind demo's dashboard, drawn by the CeeSafe renderer](https://raw.githubusercontent.com/CeeSafe/Clay/main/docs/dashboard.png)

## What it does

- **Layout:** a port of Clay 0.14's layout engine, checked against the C original by a differential fuzzer. Flex
  rows and columns, grow and fit sizing, wrapping, floating elements, scrolling and transforms.
- **Rendering:** a software renderer written to match ThorVG's output.
  - Anti-aliased paths, gradients, images, shadows, blur, filters and every CSS blend mode.
  - TrueType text, read and rasterized by the toolkit itself.
- **Speed:**
  - **Retina:** frames are drawn at the display's pixel ratio.
  - **Incremental:** only what changed between frames is redrawn, and the result is byte-identical to a full
    redraw. A 2× frame costs 2–4 ms instead of 64.
- **Controls:** buttons, text input, sliders, tabs, dropdowns, lists, a context menu and tooltips, with focus and
  transitions.

## What it uses in CeeSafe

- **Native windows:** lib/std's `Window` ([type-system.md §26.13](../spec/type-system.md)), with AppKit on macOS,
  Win32 on Windows and X11 on Linux. ClayWind's desktop host presents only the boxes that changed
  (`Window.presentChanged`). The program itself has no `extern "C"`.
- **Regions:** the renderer and the UI borrow their memory from a `FixedBufferAllocator`, and the borrow checker
  tracks those loans across the whole frame.
- **Generic methods and `dyn` interfaces:** `Ui.endFrame<M: TextMetrics>` takes any text measurer, including the
  renderer's own font engine, and every host's `Platform` is passed as a `dyn Platform`.
- **Hosts on the standard library:** sockets and async I/O for a browser host (HTTP and WebSocket servers written in
  CeeSafe), and the console for a terminal host.

Several language changes came from this port, or from work it started:
- **The port's own findings:** generic methods, arrays of views and `dyn I` as a bound.
- **Follow-ups:** static view tables, and, found while testing them, a soundness fix for `static mut` and stricter
  rules for `static` in signatures.
- **What the hosts needed:** native windows and `presentChanged`.

Each has a decision record, ADRs 0063 to 0070.

## Running it

With [ceesafec 0.14.0 or later](./installation.md):

```sh
git clone https://github.com/CeeSafe/Clay.git
cd Clay/claywind-hosts/desktop
ceesafec build
./out/desktop
```

The same demo also runs headless (for screenshots and tests), in a browser (`claywind-hosts/browser`) and in a
terminal (`claywind-hosts/terminal`). The repository's README describes each host and the test suite.
