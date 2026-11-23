# CeeUI: a UI toolkit in CeeSafe

[CeeUI](https://github.com/CeeSafe/CeeUI) is a Tailwind-flavoured immediate-mode UI toolkit written in CeeSafe. It
began as ClayWind, a port of a C toolkit built on the layout model of
[Clay by Nic Barker](https://github.com/nicbarker/clay), and was renamed when it had grown well past that. It is a
working example of what CeeSafe is for: a program that does everything itself, from layout to pixels, and runs on
macOS, Windows, Linux, iOS, Android and the web.

![The CeeUI demo's dashboard, drawn by the CeeSafe renderer](https://raw.githubusercontent.com/CeeSafe/CeeUI/main/docs/dashboard.png)

## What it does

- **Layout:** a port of Clay 0.14's layout engine, checked against the C original by a differential fuzzer. Flex
  rows and columns, grow and fit sizing, wrapping, floating elements, scrolling and transforms.
- **Rendering:** a software renderer written to match ThorVG's output, and GPU backends (Metal, Vulkan, Direct3D
  12) in the GPU host.
  - Anti-aliased paths, gradients, images, shadows, blur, filters and every CSS blend mode.
  - Only what changed between frames is redrawn, and the result is byte-identical to a full redraw.
- **Text:** TrueType and OpenType fonts read and rasterized by the toolkit itself: OpenType shaping, Indic scripts,
  Arabic joining, bidirectional lines, colour emoji, and the system's fonts as fallbacks.
- **Controls:** buttons, text fields and editors, sliders, tabs, dropdowns, lists, a virtual list, a data grid,
  charts, menus and tooltips, with focus and transitions.
- **Screen readers:** VoiceOver, NVDA, Orca and TalkBack read an app's controls, lists, tabs, menus and tables, and
  a web page gives them as ARIA.

## What it uses in CeeSafe

- **Windows:** lib/std's `Window` ([type-system.md §26.13](../spec/type-system.md)): AppKit, Win32, Wayland and
  X11, UIKit and Android. CeeUI's desktop host presents only the boxes that changed (`Window.presentChanged`), and
  tells a screen reader what a window holds (`Window.setAccessNodes`).
- **Regions:** the renderer and the UI borrow their memory from a `FixedBufferAllocator`, and the borrow checker
  tracks those loans across the whole frame.
- **Generic methods and `dyn` interfaces:** `Ui.endFrame<M: TextMetrics>` takes any text measurer, including the
  renderer's own font engine, and every host's `Platform` is passed as a `dyn Platform`.
- **Hosts on the standard library:** sockets and async I/O for a browser host (HTTP and WebSocket servers written in
  CeeSafe), the console for a terminal host, and threads for background work.
- **A C API:** the library built with `--emit lib`, with bindings for Swift, Kotlin and Rust.

Many language and library changes came from this work: generic methods, arrays of views and `dyn I` as a bound
(ADRs 0063 to 0070), and since then the window's text input, touch, safe areas, menus, file dialogs and
accessibility. CeeUI's `CEESAFE-FINDINGS.md` lists each finding and how it was resolved.

## Running it

With [ceesafec](./installation.md), the release CeeUI's README names:

```sh
git clone https://github.com/CeeSafe/CeeUI.git
cd CeeUI/ceeui-hosts/desktop
ceesafec build
./out/desktop
```

The same demo also runs headless (for screenshots and tests), in a browser (`ceeui-hosts/browser`), in a terminal
(`ceeui-hosts/terminal`), on phones and as a web page. CeeUI's own guide (`site/guide` in its repository) starts
from a new app and covers layout, styles, the controls, text, phones, the web and accessibility.
