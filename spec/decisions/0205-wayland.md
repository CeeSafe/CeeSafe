# ADR 0205 — Windows on Wayland

- Status: **Accepted** by the owner, 2026-11-22 ("I accept your recommendation 1-3": X11 stays the default until
  step 2, libdecor draws the title bar on GNOME, a release per step), for ClayWind's roadmap phase P5 (platform
  completeness), whose last open item is Wayland. Steps 1 and 2 are implemented (§7, §8); step 3, CeeSafe's own title bar, is next.
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
| Title bar | the window manager's | `zxdg_decoration_manager_v1` server-side where there is one (KDE, wlroots); else `libdecor-0.so.0` with its cairo plugin (GNOME; §7); without either, none (the window still moves with Super and a drag); CeeSafe's own in step 3 |
| Keys | Xlib's keysyms | xkbcommon over the compositor's keymap; repeat by the runtime at the seat's `repeat_info` |
| Text input | XIM | `zwp_text_input_v3`: the composition, the commit, the caret area from `setTextInputArea` (ADRs 0141, 0157) |
| Pointer, scroll | core events | `wl_pointer` frames; `axis_value120` for wheels, pixels for touchpads |
| Touch | none | `wl_touch` (ADR 0155) |
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
3. **CeeSafe's own title bar** where the compositor draws none (the owner, 2026-11-22: "I prefer the cairo plugin.
   Then we draw CeeSafe's own title bar"), in place of libdecor's, with its own ADR section.

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

## 6. Questions for the owner (answered: the proposals, all three)

1. **The default**: X11 until step 2 (proposed), or Wayland from step 1?
2. **The title bar on GNOME**: libdecor (proposed), or a CeeSafe-drawn one now?
3. **Releases**: one per step (proposed), or both steps in one?

## 7. Step 1 as implemented (2026-11-22)

`runtime/hosted/ceesafe_rt_window.c` (a Wayland part in the Linux and FreeBSD section), `lib/std/window.cee`
(`WindowHandle.Wayland`, the errors' notes), type-system.md §26.13, `tools/window_check.py`.

- **As §2 has it**, chosen by `CEESAFE_WINDOW_SYSTEM=wayland`; X11 is the default. A Wayland window keeps the X11 one's
  record for what both have (its title, its AT-SPI serial and focus), with a display of NULL, so AT-SPI, the file
  dialogs and the rest serve both. One connection serves the process; the waits poll it with the wake pipes and
  the accessibility bus.
- **The title bar on GNOME is libdecor's cairo plugin** (the owner, 2026-11-22: "I prefer the cairo plugin. Then we
  draw CeeSafe's own title bar"). libdecor chose its GTK plugin, which runs GTK 3 in the program, and GTK aborts a
  program whose icons it cannot load: Fedora 45's GTK loads them through a program it finds on `PATH`, so with an
  empty `PATH` (`window_check`'s dialog case) the program stopped in `Gtk:ERROR`. libdecor takes the plugins of
  `LIBDECOR_PLUGIN_DIR`; the runtime points it at `$XDG_RUNTIME_DIR/ceesafe-libdecor`, which holds a link to the
  cairo plugin beside libdecor's own, unless the environment names a folder or there is no cairo plugin. The cairo
  plugin draws a plain title bar with cairo and pango, and no GTK is loaded. `NO_AT_BRIDGE=1` is set too, should
  libdecor still take the GTK one, whose accessibility bridge would be a second application for readers.
- **Found on the way**, each fixed:
  - **A new window did not get the keyboard** under GNOME, which gives it only to a window its launcher vouches for:
    the runtime passes `XDG_ACTIVATION_TOKEN` to `xdg_activation_v1` and removes it from the environment.
  - **`Focus(true)` never came**: every window starts focused (as on the other systems, where it is true); a
    Wayland one starts without the focus, and the compositor's `wl_keyboard.enter` gives it.
  - A frame's pixels are R, G, B, A in memory, which is `XBGR8888`; the runtime uses it when the compositor offers it
    (mutter does) and swaps R and B into `XRGB8888` when not.
- **Checked on Fedora 45** (GNOME 51 on Wayland, mutter's protocols as §1 lists them):
  - `window_check --display` with `CEESAFE_WINDOW_SYSTEM=wayland`: 77 checks, 0 problems (the clipboard's and XDND's
    are not run: step 2); without it, on X11: 77, 0. `atspi_check` on Wayland: 53, 0, the registry's ping included.
  - Scale, a 300 by 200 window: at the display's 1.25 and 2, `scale()` is 1.25 and 2 on Wayland and 1 on X11
    (Xwayland stretches it); the size in points is 300 by 200 in each.
  - Keys, pressed through mutter's remote desktop (`xdotool`'s XTEST reaches X windows only): one held for 1.5 s
    gives 1 down and 34 repeats, at GNOME's 500 ms delay and 30 ms interval; the key that is Y on a US layout types
    `y`, and on the German layout `z`.
  - ClayWind's component demo and C example: 81 elements read through AT-SPI; Orca 51, Tab by Tab: "Dark not
    selected radio button", "Open in a window button", "Subscribe to updates check box not checked", "Email
    notifications check box checked", "System selected radio button".
- **Not checked**: KDE and wlroots compositors, where the title bar is the compositor's; FreeBSD; touch (no touch
  screen); two monitors of different scale.

## 8. Step 2 as implemented (2026-11-22)

`runtime/hosted/ceesafe_rt_window.c`, `lib/std/window.cee` (the errors' notes), type-system.md §26.13,
`tools/window_check.py`.

- **Wayland is the default** where there is one: the first open takes `CEESAFE_WINDOW_SYSTEM` when it says `wayland`
  or `x11`, and else connects to the compositor `WAYLAND_DISPLAY` names, keeping X11 when that fails (a stale
  variable, no library). Checked on the owner's Fedora 45 PC by a probe that prints its handle's kind: no setting
  `wayland`; `x11` asked `x11`; `wayland` asked `wayland`; a `WAYLAND_DISPLAY` with no compositor behind it `x11`;
  that and no `DISPLAY` `NoDisplay`.
- **The clipboard**, `wl_data_device`: a source offering the text as `text/plain;charset=utf-8`, `UTF8_STRING`,
  `text/plain`, `TEXT` and `STRING`, made the selection with the serial of the last key, button or keyboard
  focus; the selection's offer read through a pipe (a second, 16 MiB at most); the window's own text while the
  selection is still its source. A compositor gives the selection only with an input's serial, so before any input
  `setClipboardText` is `Resources`, and `window_check`'s clipboard case, whose window takes none, is not run on
  Wayland. Checked by hand, keys pressed through mutter's remote desktop into ClayWind's C example: Ctrl+A and Ctrl+C
  in its name field, then `wl-paste`: `World`; `wl-copy "Pasted ✓ in"`, then Ctrl+A and Ctrl+V: the field reads
  `Pasted ✓ in` through AT-SPI.
- **Drops**: a drag offering `text/uri-list` is accepted with the copy action, its list read at its entry (the files
  counted, ADR 0150's `dragOver`) and at the drop (the paths, through X11's URI parser, which both share); other
  drags are refused. Checked with the owner dragging a file from GNOME Files onto a probe: `drag over: 1 files`,
  then the drop with `/home/kutty/drop-test/first.txt`; the protocol trace shows the offer's types, the accept, the
  copy action and the list read.
- **The input method**, `zwp_text_input_v3`: enabled while the window it entered has text input on, with the
  field's kind as its content type (Text normal, Number digits, Decimal number, Email email, Url url, Password
  password with the sensitive-data and hidden-text hints) and the caret's area as its cursor rectangle; each `done`
  gives what was committed as `Text` and the composition as `Composition`. The runtime sends no surrounding text, so
  an input method has none to delete. Checked with IBus's Anthy: `nihon` typed in the C example's name field
  composes (the field's text is still `World`), and Enter commits it: `Worldにほん`.
- **Found on the way, on X11**: `window_check`'s clipboard case failed once in five runs on the owner's PC, the
  program ended by "X Error of failed request: BadWindow". GNOME's Xwayland bridge asks an X11 program for the
  clipboard's text to give it to Wayland's programs, and its requestor window can be gone when the answer reaches
  it; the runtime installed no X error handler, so Xlib's own ended the process. The runtime now installs one when it
  loads libX11: BadWindow is ignored (another client's window gone is not the program's failure), and every other
  error goes to the handler there was before, the program's own or Xlib's. Three runs after it: 77 checks, 0
  problems each.
- **Not checked**: an input method on KDE (its `text-input-v1`/`v2` are not taken; `v3` only); a drag from an X11
  program into a Wayland window (Xwayland's bridge is the compositor's); the clipboard's `INCR`-sized texts against
  X11 programs.
