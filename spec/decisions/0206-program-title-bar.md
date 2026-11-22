# ADR 0206 — A title bar the program draws

- Status: **Accepted** by the owner, 2026-11-22 ("Go ahead I'm with you in all 1-3": the layout, Wayland then
  Windows then macOS and X11, ClayWind's demo with its own bar on each system as it comes), as ADR 0205's step 3 (the
  owner: "I prefer the cairo plugin. Then we draw CeeSafe's own title bar").
- Scope: `lib/std/window.cee` (an open option, a title bar's layout, three window actions), `runtime/hosted/
  ceesafe_rt_window.c` (Wayland first; Windows and macOS later steps), type-system.md §26.13, `tools/window_check.py`.
  No language or compiler change.

## 1. Context

- **Who draws a window's frame today.** macOS, Windows and X11's window managers draw a title bar for every window.
  A Wayland compositor may not: GNOME draws none, and since ADR 0205 libdecor's cairo plugin draws one inside the
  program. It is plain (cairo's own buttons and fonts, not the desktop's), it brings cairo and pango into every
  program, and it cannot look like the program it frames.
- **The runtime cannot draw text.** A title bar shows the title. The runtime has no font engine and no shaping; the
  programs that draw a UI (ClayWind) have both, with their own fonts and theme.
- **What GPUI does** (Zed, ClayWind's model): on Linux and Windows the app draws its own title bar, as part of its
  UI, in its own look, and the platform layer gives it what only the system can do: moving and resizing the window by
  the pointer, minimizing, maximizing, the window menu, and on Windows the hit tests that snap layouts need.

## 2. Decision

### 2.1 Asking for it

- **`Window.openStyled(title, width, height, surface, style: WindowStyle)`**, `WindowStyle` being `System` (what
  `open` and `openWith` give, which do not change) or **`ProgramTitleBar`**: the window has no title bar of the
  system's or libdecor's; the program draws one at the top of its content. Where the system does not allow it, the
  window is as `System` gives it, and `programTitleBar()` says which it got.
- **`w.programTitleBar() -> bool`**: whether the program draws the title bar. A program draws it only then.

### 2.2 Telling the runtime where it is

- **`w.setTitleBar(bar: WindowTitleBar)`**, each frame its layout changes: `height: f32`, the bar's height in points
  from the content's top, and four boxes in points: `minimize`, `maximize`, `close`, and `menu` (a box of 0 by 0 is
  none). A press in the bar but in no box moves the window, a double press toggles maximized, a secondary press
  shows the system's window menu; the program draws the buttons and acts on their presses itself (they are events
  as any other). Windows needs the boxes to answer its hit tests (snap layouts open over `maximize`); Wayland needs
  the bar to start the compositor's move with the press's serial.
- **Resizing** by the edges is the runtime's: a press within 6 points of an edge or a corner resizes, and the pointer
  takes the resize shape there, unless the window is maximized, fullscreen or tiled.

### 2.3 What only the system can do

- **`w.minimize()`**, **`w.toggleMaximized()`**, and **`w.maximized() -> bool`** (a change of it is a `Resized`).
  Closing is the program's own: it drops the window.

### 2.4 Order of work

1. **Wayland**: no libdecor for such a window; the move, resize, menu and maximize through `xdg_toplevel`; the
   tiled and maximized states from its configure. libdecor's cairo plugin stays for `System` windows on compositors
   that draw no title bar.
2. **Windows**: the non-client area removed (`WM_NCCALCSIZE`), `WM_NCHITTEST` answered from the layout.
3. **macOS**: a full-size content view with a transparent title bar; the traffic lights stay the system's, and
   the layout's `minimize`, `maximize` and `close` are left out there. X11: `_MOTIF_WM_HINTS` without
   decorations, the move and resize through `_NET_WM_MOVERESIZE`.
4. **ClayWind**: a `TitleBar` component that draws the title and the buttons in the theme and gives the window its
   layout each frame.

## 3. Alternatives considered

- **The runtime draws a title bar of its own** (a subsurface above the content): it would need a font engine (FreeType
  and HarfBuzz through `dlopen`, as libdecor's cairo and pango are) to draw a title nobody can style, which is what
  libdecor's plugin is already.
- **Actions instead of a layout** (`beginMove()` on a press, `beginResize(edge)`): enough for Wayland and X11, not for
  Windows, whose snap layouts and Aero Snap come from the system's hit test, before the program sees the press.
- **Choosing after the window opened** (`setStyle`): switching a mapped Wayland window between libdecor's frame and
  none means remaking its toplevel; asked at the open, nothing is remade.

## 4. Consequences

- On GNOME, a ClayWind window can look like ClayWind, with no cairo or pango loaded; on Windows it can have Windows
  11's rounded, snapping frame with ClayWind's bar in it.
- A program that asks for `ProgramTitleBar` and draws no bar has a window the user cannot move but by the keyboard
  (Super and a drag on GNOME): the program's choice, as a full-screen game's.
- Programs that call `open` or `openWith` see no change.

## 5. What an implementation must show

- On Fedora 45's GNOME (Wayland): a press in the bar moves the window, a double press maximizes and restores it, a
  secondary press shows GNOME's window menu, the edges resize it, `minimize()` and `toggleMaximized()` act, and
  `maximized()` follows; libdecor is not loaded for it; a `System` window still has libdecor's bar.
- `window_check`: the API in `CEESAFE_WINDOW_SCRIPT` runs (the layout logged, the actions as script lines).
- ClayWind's demo with its own title bar, checked by the owner.

## 6. Questions for the owner (answered: the proposals, all three)

1. **The API**: a layout the program gives (proposed, §2.2), or actions it calls (§3)?
2. **The order**: Wayland, then Windows, then macOS and X11 (proposed), or only Wayland for now?
3. **ClayWind's demo**: its own title bar on every system once there (proposed), or on Linux only?
