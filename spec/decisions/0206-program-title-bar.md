# ADR 0206 — A title bar the program draws

- Status: **Accepted** by the owner, 2026-11-22 ("Go ahead I'm with you in all 1-3": the layout, Wayland then
  Windows then macOS and X11, ClayWind's demo with its own bar on each system as it comes), as ADR 0205's step 3 (the
  owner: "I prefer the cairo plugin. Then we draw CeeSafe's own title bar").
- Scope: `lib/std/window.cee` (an open option, a title bar's layout, three window actions), `runtime/hosted/
  ceesafe_rt_window.c` (Wayland, then Windows, then macOS and X11), type-system.md §26.13, `tools/window_check.py`.
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

- **`w.systemButtons() -> WindowArea`** (added at step 3, the owner's choice: "systemButtons()"): the area, with
  their margins, that the system's own buttons take in the bar, which the program leaves clear, drawing no
  minimize, maximize or close of its own then. On macOS, the traffic lights; 0 by 0 elsewhere, where the program
  draws them. Without it a program could not know where the lights are, nor that it should not draw buttons.

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

## 7. Step 1 as implemented: Wayland (2026-11-22)

`lib/std/window.cee` (`WindowStyle`, `WindowArea`, `WindowTitleBar`, `openStyled`, `programTitleBar`, `setTitleBar`,
`minimize`, `toggleMaximized`, `maximized`), `runtime/hosted/ceesafe_rt_window.c`, type-system.md §26.13,
`tools/window_check.py`.

- **As §2 has it.** `openStyled` passes the style above the surface's number, so the runtime's open keeps its
  signature. A Wayland window asking for the program's bar has a plain `xdg_toplevel`: no libdecor frame, and where
  the compositor offers decorations (`zxdg_decoration_manager_v1`) it is asked for client-side ones, the program's
  bar being granted only if it agrees. The configure's states give `maximized()` and whether the window is tiled or
  fullscreen. The runtime takes the presses §2.2 names (`xdg_toplevel`'s `move`, `resize` with the edge,
  `show_window_menu`, `set_maximized` or `unset_maximized`), with the serial of the press, and the program does not
  see them; over an edge the pointer takes cursor-shape-v1's resize shape.
- **libdecor is loaded at the first window that needs it**, not at the connection: a program whose windows all draw
  their own bar loads neither libdecor nor cairo nor pango. Checked: the probe's process maps
  `libwayland-client.so.0` and no `libdecor`.
- **The name**: the layout's boxes are `WindowArea`s, in points as `f32`; `WindowBox` was taken, `presentChanged`'s in
  pixels as `u32`. The first draft reused the name, and the compiler took the first definition: lib/std's duplicate
  went unreported because the embedded library is not checked when a program is built (`ceesafec check lib/std`
  and the contract suite do check it).
- **Script runs**: a scripted window keeps the layout and has no system frame, so `programTitleBar()` is false and
  the actions do nothing; `window_check` runs the API there (§5's "the layout logged" is not done: no other
  window call writes the script's log either).
- **Checked on Fedora 45** (GNOME on Wayland, the VM): a probe window draws a dark bar of 36 points with three
  buttons. Its log: `program title bar true`; the maximize button took it from 480 by 320 to 1024 by 736,
  `maximized true`, and back; a double press in the bar maximized it with no press reaching the program; the
  minimize button minimized it; the close button ended it. The owner, at the VM: the bar moves the window, a double
  press maximizes and restores it, a secondary press shows GNOME's window menu, the edges and corners resize it with
  the resize pointer ("Yes, done all. Work as you said above"); the move, the menu and the resizes are the owner's
  check alone, the probe logging a size only when its width changed, which its log never shows but for maximizing. `window_check`: the scripted case, and a real window
  says `bar true false` on Wayland and `bar false false` elsewhere.
- **Not yet**: steps 2 to 4 (Windows, macOS and X11, ClayWind's `TitleBar`).

## 8. Step 2 as implemented: Windows (2026-11-22)

`runtime/hosted/ceesafe_rt_window.c` (the Win32 part), type-system.md §26.13, `tools/window_check.py`.

- **The frame stays Windows'.** The window keeps `WS_OVERLAPPEDWINDOW`; `WM_NCCALCSIZE` takes the caption away, the
  client area keeping the frame's left, right and bottom borders (`GetSystemMetricsForDpi`'s frame and padding at the
  window's DPI) and reaching the window's top; a maximized window, which overhangs the screen by the frame, keeps
  its top off too, and a minimized one is left to Windows. So Windows still draws the borders, the shadow and
  Windows 11's rounded corners, and still resizes and snaps the window. The window is opened at the content's size
  plus those borders, with `SWP_FRAMECHANGED`.
- **`WM_NCHITTEST`** answers from the layout: Windows' own answer for the borders; the top border, which the
  caption's removal gave the client, as `HTTOP` (and its corners); in the bar, the maximize box as `HTMAXBUTTON`,
  the other boxes as the client's, the rest as `HTCAPTION`, so Windows moves the window by it, maximizes it on a
  double click, shows its menu on a secondary press and snaps it.
- **The maximize box is the frame's** to Windows, so the program would not see the pointer there: `WM_NCMOUSEMOVE`
  and `WM_NCLBUTTONDOWN`/`UP` over it are given to the program as moves and presses (Windows' classic button is
  not drawn), and leaving it (`WM_NCMOUSELEAVE`, tracked) as a move off it.
- **The actions**: `minimize` is `ShowWindow(SW_MINIMIZE)`, `toggleMaximized` `SW_MAXIMIZE` or `SW_RESTORE`;
  `maximized()` follows `WM_SIZE`'s `SIZE_MAXIMIZED`, for every window. A minimized window's `WM_SIZE` is not a
  resize: the content keeps its size (with the caption gone, the minimized frame had a client area of its own, and
  a program saw 144 by 20).
- **Checked on Windows 11 (ARM64, Parallels), in the desktop session**, by a probe drawing a 36-point bar with three
  buttons and a script asking its window and pressing with real mouse events: the client 480 by 320 in a 496 by 328
  window (no caption); hit tests in the bar 2 (`HTCAPTION`), on the maximize box 9 (`HTMAXBUTTON`), on the minimize
  and close boxes and the content 1, at the top edge 12 (`HTTOP`); the maximize box's press reached the program,
  which maximized the window (1440 by 852) and, pressed again, restored it; minimize minimized it; close ended it.
  The screenshots show Windows 11's corners and shadow around the program's bar. `window_check --display`: a real
  window says `bar true false`.
- **ClayWind's demo** on the VM, built with this runtime, with its own bar (ClayWind's `TitleBar`), checked by the
  owner ("Perfect!"): moved and maximized by the bar, the snap layouts over its maximize button, its three buttons,
  resized by the edges.
- **`window_check --display`**: on the owner's x64 laptop (MinGW gcc) 78 checks, 0 problems; on the ARM64 VM (LLVM
  clang) its drop case's helper, a C program, did not build: `window_check` ran `clang` outside Visual Studio's
  environment, where `windows.h` is not found (ceesafec sets that environment up itself, tooling.md §8.2), and did
  not name `user32`, which MinGW links by itself. It now sets the environment up as ceesafec does (vswhere,
  vcvarsall.bat for the machine), asks Windows for the machine (`IsWow64Process2`: an x64 Python emulated on ARM64
  is told AMD64, and took x64's libraries; the process's pseudo-handle is passed as a pointer, not ctypes' default
  32-bit int, or the call fails), and names `-luser32`. On the VM then: 78 checks, 0 problems.
- **Not checked**: a second monitor at another DPI; Windows 10.

## 9. Step 3 as implemented: macOS and X11 (2026-11-22)

`runtime/hosted/ceesafe_rt_window.c` (the AppKit and X11 parts), `lib/std/window.cee` (`systemButtons`),
type-system.md §26.13, `tools/window_check.py`.

### 9.1 macOS

- **The window**: `NSWindowStyleMaskFullSizeContentView`, `titlebarAppearsTransparent`, the title hidden (it stays
  the window's, for the Window menu and Mission Control). The content fills the window; the program's bar is drawn
  under AppKit's transparent one.
- **The traffic lights stay the system's** (§2.2's `systemButtons`): they are centered in the program's bar (its
  height from `setTitleBar`, AppKit's title bar's until then), as far in from the left as from the top (7 points at
  least), and placed again when AppKit lays them out anew (each resize). A bar taller than AppKit's title bar keeps
  them inside it. `systemButtons()` is the area from the content's left to the lights' right with that margin, as
  high as the bar; in full screen, where AppKit hides them, it is none.
- **Presses**: the runtime sees every event before AppKit (`cee_native_pump`). A primary press in the bar outside
  the program's boxes and the lights moves the window (`performWindowDragWithEvent:`), and a double press does the
  user's `AppleActionOnDoubleClick` (zoom by default, minimize, or nothing); the program sees neither. A press on a
  light or in the top 4 points (the frame's resize edge) goes to AppKit alone. A press in a program's box goes to the
  program alone, not to AppKit's title bar, which would move the window by it. There is no window menu on macOS.
- **The actions**: `miniaturize:` and `zoom:`; `maximized()` is `isZoomed`, read at each resize, for every window.
- **Checked on this Mac (macOS 26)** with a probe and real mouse events (CGEvent) and screenshots: the lights
  centered in a 36-point bar, `systemButtons()` 0,0 82 by 36; dragging the bar moved the window by the drag (480,150
  to 580,200); a press in the program's menu box reached it and did not move the window; a double press zoomed
  (1440 by 799, `maximized` true) and another restored; the top edge resized (320 to 381 high) with no press to the
  program; the green light entered full screen (`systemButtons()` none) and, again, left it (the area back); yellow
  minimized; red gave `Closed`. `window_check --display`: 80 of 80.

### 9.2 X11

- **The frame**: `_MOTIF_WM_HINTS` with no decorations, only when the window manager says (`_NET_SUPPORTED`) that it
  moves windows for a client (`_NET_WM_MOVERESIZE`); without one the window keeps the system's frame, and
  `programTitleBar()` is false.
- **Presses**, as on Wayland: at an edge (6 points, not while maximized) the primary button resizes, by
  `_NET_WM_MOVERESIZE` with the edge's direction; in the bar outside the boxes it moves the window
  (`_NET_WM_MOVERESIZE_MOVE`), a second within 0.4 s toggles maximized, and the secondary shows the window menu
  (`_GTK_SHOW_WINDOW_MENU`, GNOME's and KDE's). The press's implicit grab is let go first (`XUngrabPointer`), so that
  the window manager can take the pointer. The pointer takes the edge's resize shape (the cursor font's sides and
  corners).
- **The actions**: `WM_CHANGE_STATE` (iconic) and `_NET_WM_STATE` (toggle both maximized states); `maximized()` is
  read from `_NET_WM_STATE` at each `PropertyNotify` (the window now selects `PropertyChangeMask`), for every window.
- **Checked on the Fedora 45 VM** (GNOME, Xwayland, `CEESAFE_WINDOW_SYSTEM=x11`) with the probe and GNOME's real
  pointer (Mutter's RemoteDesktop; Xwayland takes no synthetic input while the real pointer is elsewhere): no frame
  (`_NET_FRAME_EXTENTS` none); dragging the bar moved the window by the drag (272,240 to 372,290); a double press
  maximized it (1024 by 736, `_NET_WM_STATE` both maximized, `maximized()` true); the program's maximize box restored
  it; the right edge resized it (480 to 562 wide); a secondary press in the bar was the runtime's (the press after it
  closed the menu); the minimize box made it iconic. `window_check --display`: 79 of 79 on Wayland and on X11.
- **Also**: the cairo plugin's lookup (ADR 0205) passed a function pointer to `dladdr` as an object pointer, which
  ISO C does not convert (`-Wpedantic` warned in Linux builds); its address is copied
  now.
- **Not checked**: the edge's resize shape (no screenshot of Xwayland's pointer here); window managers other than
  Mutter.
