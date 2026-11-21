# ADR 0205 — Windows on Wayland

- Status: **Proposed**, 2026-11-22, for ClayWind's roadmap phase P5 (platform completeness), whose last open item is
  Wayland. Not yet accepted.
- Scope: `runtime/hosted/ceesafe_rt_window.c` (a Wayland part beside the X11 one, for Linux and FreeBSD),
  `lib/std/window.cee` (one `WindowHandle` variant, and words in the errors' notes), type-system.md §26.13,
  `tools/window_check.py` and `tools/atspi_check.py` (run under both). No language or compiler change.

## 1. Context

- **Linux windows are X11 only** (ADR 0069 §5 question 1 left Wayland "later"; every window ADR since says "Wayland,
  as before"). On a Wayland desktop they run through Xwayland, the compositor's X server. That works, and is what
  ClayWind's checks on Fedora 45 used (ADR 0164 §11), but:
  - **Scale.** An X11 window has one scale for the whole display and none of its own. On a fractional or
    per-monitor scale GNOME draws it at 1 and stretches it: blurred text, the one thing a UI toolkit must not blur.
    `Window.scale()` is 1 under Xwayland whatever the monitor.
  - **Input methods** go through XIM, which GNOME's and KDE's input methods serve only in part (pre-edit placement,
    candidate windows); Wayland's `text-input-v3` is what they serve fully.
  - **Pacing.** X11 has no "the compositor shows your frame now"; Wayland's frame callbacks are that.
  - **The direction.** GNOME no longer has an X11 session (Fedora 45's desktop is Wayland only); Xwayland stays for
    old programs.
    A toolkit that competes with GPUI, which is native on Wayland, cannot be an old program there.
- **What a Wayland compositor offers** (Fedora 45, GNOME 51's mutter, measured with `wayland-info`):
  `wl_compositor` 6, `wl_shm` 2, `xdg_wm_base` 7, `wl_seat` 10, `wl_data_device_manager` 3,
  `zwp_text_input_manager_v3`, `wp_fractional_scale_manager_v1`, `wp_viewporter`, `wp_cursor_shape_manager_v1` 2,
  `xdg_activation_v1`, `wp_presentation`. **Not** `zxdg_decoration_manager_v1`: GNOME draws no title bar for a
  Wayland window, the client draws its own. Installed with the desktop: `libwayland-client.so.0`,
  `libxkbcommon.so.0`, `libdecor-0.so.0` (0.2.5, the library SDL, GLFW and Qt use for that title bar).
- **Wayland gives a client less than X11**: no global coordinates (a window does not know where it is on the
  screen), no key repeat (the client repeats), no keymap decoding (the client runs xkbcommon over the keymap the
  compositor sends), and a clipboard offered only by the window with the keyboard's focus.

## 2. Decision

### 2.1 Which system a window uses

1. At the first `open`, the runtime chooses **Wayland when `WAYLAND_DISPLAY` is set** (or `WAYLAND_SOCKET`),
   `libwayland-client.so.0` and `libxkbcommon.so.0` load, and the compositor has `wl_compositor` 4 or later,
   `wl_shm`, `xdg_wm_base` and `wl_seat`. Otherwise **X11 as today**; neither is `NoDisplay`. The choice holds for
   the process.
2. `CEESAFE_WINDOW_SYSTEM=x11` or `=wayland` in the environment makes the choice (and `wayland` without a compositor
   is `NoDisplay`), for tests and for a program a compositor serves badly.
3. Everything is loaded with `dlopen`, as libX11 and libdbus are; nothing is linked, so a program built on one
   desktop runs on the other. The protocols' interface tables (`xdg_wm_base`, `xdg_surface`, `xdg_toplevel`, the
   staged ones below) are written out in the runtime from their XML, as AT-SPI's were (ADR 0164 §11); the core
   ones are libwayland-client's own symbols.

### 2.2 What a Wayland window does, against the X11 one

| | X11 today | Wayland |
|---|---|---|
| Frames | `XPutImage` | `wl_shm` buffers in a `memfd`, two a window; `presentChanged`'s boxes as `damage_buffer`; a commit |
| `waitFrame` | a timer | the surface's frame callback, so a hidden window does not draw |
| Scale | 1 | `wp_fractional_scale_v1` with `wp_viewporter` (else `wl_surface` 6's preferred buffer scale, else the output's); `scale()` the fraction, sizes in points, buffers in pixels, as on macOS |
| Title bar | the window manager's | `zxdg_decoration_manager_v1` server-side where there is one (KDE, wlroots); else `libdecor-0.so.0` (GNOME); without either, none (the window still moves with Super and a drag) |
| Keys | Xlib's keysyms | xkbcommon over the compositor's keymap; repeat by the runtime at the seat's `repeat_info` |
| Text input | XIM | `zwp_text_input_v3`: the composition, the commit, the caret area from `setTextInputArea` (ADRs 0141, 0157) |
| Pointer, scroll | core events | `wl_pointer` frames; `axis_value120` for wheels, pixels for touchpads |
| Touch | XInput 2 | `wl_touch` (ADR 0155) |
| Cursor | font cursors | `wp_cursor_shape_v1`; without it, the compositor's arrow |
| Clipboard | `CLIPBOARD` selection | `wl_data_device`, `text/plain;charset=utf-8`, with the keyboard focus's serial |
| Drops | XDND | `wl_data_device` offers of `text/uri-list` (ADR 0150) |
| Wait, wake | poll on the X socket | `wl_display_prepare_read` and poll on the display's descriptor, the wake pipe and the accessibility bus |
| Accessibility | AT-SPI, screen coordinates | AT-SPI as is; screen coordinates are the window's (a Wayland client has no others), as GTK 4 gives |
| Menus, file dialogs | `Unsupported`; `zenity` or `kdialog` | the same |

### 2.3 The API

- **`WindowHandle.Wayland(u64, u64)`**: the `wl_display *` and the window's `wl_surface *`, for `extern "C"` code that
  draws with Vulkan or EGL (ClayWind's GPU layer). A new variant, as `UIKit` and `Android` were (ADR 0154): an
  exhaustive `match` on `WindowHandle` needs an arm for it.
- No other change: every method, event and error is the same, and `NoDisplay` now reads "no `DISPLAY` or Wayland
  compositor".

### 2.4 Order of work

1. **The window**: open, frames, scale, title bar, close, resize, focus, the pointer, keys with repeat, touch, the
   cursor, waits and wakes, `nativeHandle`, AT-SPI. `CEESAFE_WINDOW_SYSTEM=wayland` chooses it; X11 stays the
   default.
2. **Text and data**: `text-input-v3`, the clipboard, drops. Then **Wayland becomes the default** where §2.1 finds
   one.

## 3. Alternatives considered

- **Xwayland only.** What there is. Blurred text at fractional scale and a partial input method are the cost; GNOME's
  direction makes it a stopgap.
- **SDL or GLFW underneath.** Both do this, with libdecor. They would be the runtime's first dependency on a C library
  it does not load itself, for one system, and their event models are not `Window`'s.
- **Linking libwayland-client at build time.** Every Linux program with a window would then need the library to start,
  on X11-only systems too.
- **Drawing the title bar ourselves** where the compositor will not. It is what libdecor's fallback does, and what
  every toolkit with its own look ends up wanting; a CeeSafe one would be pixels in the runtime, with themes to
  follow. Proposed later, if libdecor's falls short.
- **Wayland as the default from step 1.** Until step 2, a Wayland window has no input method and no clipboard, which
  X11 windows have.

## 4. Consequences

- On a Wayland desktop a CeeSafe window is sharp at any scale, follows a window between monitors of different scale,
  and draws when the compositor is ready for it.
- `window_check --display` and `atspi_check` run twice on Linux, once per system.
- On GNOME the title bar is libdecor's, which looks like GTK's; on KDE and wlroots it is the compositor's.
- A Wayland window cannot know where it is on the screen. Nothing in `Window` gives a position; AT-SPI's screen
  coordinates become the window's (readers on Wayland expect that).

## 5. What an implementation must show

- `window_check --display` with `CEESAFE_WINDOW_SYSTEM=wayland` and with `=x11` on Fedora 45: the same checks pass,
  but those that are X11's (XDND's protocol) or Wayland's.
- At scale 1.25 and 2: `scale()` says so, a frame is that many pixels, and text is not stretched.
- Keys: repeat at the seat's rate; a layout other than US (the keymap's own symbols and text).
- `atspi_check` under Wayland; Orca reads ClayWind's demo there.
- ClayWind's demo and C example on Wayland, with libdecor's title bar on GNOME.
- Not checkable here: KDE and wlroots compositors (no machine; a second VM could be).

## 6. Questions for the owner

1. **The default**: X11 until step 2 (proposed), or Wayland from step 1?
2. **The title bar on GNOME**: libdecor (proposed), or a CeeSafe-drawn one now?
3. **Releases**: one per step (proposed), or both steps in one?
