# ADR 0146 — Menus and file dialogs

- Status: **Accepted** by the owner, 2026-11-15. ClayWind's roadmap step P3.5 is "menus and file dialogs", and
  `Window` (type-system.md §26.13) has neither. The owner said "Go ahead with step 5"; the design below was the lead's,
  chosen as defaults, and the questions it settles are listed in §5 so that the owner can revisit them. Implemented on
  branch `adr/0146-menus-file-dialogs`, released later.
- Scope: `lib/std/window.cee`, `runtime/hosted/ceesafe_rt_window.c`, type-system.md §26.13, `spec/api/std.md`
  (generated), `tools/window_check.py`, the corpus. No language, emitter or interpreter change.

## 1. Context

- **Menus.** A desktop program's commands live in its menus: macOS has one menu bar for the application, at the top
  of the screen, whose first menu is the application's (About, Hide, Quit); a Windows window has its own menu bar;
  X11 has no menu bar of its own, and a program draws its menus (ClayWind does, as Zed's GPUI does on Linux).
- **Shortcuts.** On macOS a menu item's key equivalent is how a shortcut works: AppKit gives a key down to the main
  menu's `performKeyEquivalent:` before the window sees it, and the item's action runs. The runtime's pump does not
  hand key events to `sendEvent:` (an unhandled key beeps), so without this ADR no menu shortcut could work. On
  Windows, a menu item's shortcut is text after a tab, right-aligned; an accelerator table would make it act.
- **File dialogs.** Opening and saving files needs the system's dialogs: NSOpenPanel and NSSavePanel, Windows' common
  item dialogs (`IFileOpenDialog`, `IFileSaveDialog`, COM), and on Linux `zenity` (GTK) or `kdialog` (KDE), the
  programs that desktop environments ship for this.
- **Tests.** The scripted window (ADR 0069 §2.3) lets CI test every window feature on every system. Menus and dialogs
  need script lines for what the user chooses.

## 2. Decision

### 2.1 Menus (lib/std)

```ceesafe
var menu = Menu.create();
menu.addMenu("File")?;
menu.addItem(OPEN, "Open…", MenuShortcut { key: WindowKey.Character(0x6F), modifiers: command })?;
menu.addSeparator()?;
menu.addItem(QUIT, "Quit", null)?;
menu.addMenu("View")?;
menu.addItem(WRAP, "Wrap Lines", null)?;
menu.setChecked(WRAP, true);
window.setMenu(borrow menu)?;
loop {
    window.waitEvent(1000);
    // nextEvent, then:
    loop {
        match (window.nextMenuChoice()) {
            Some(id) => { /* act on the item `id` */ }
            null => { break; }
        }
    }
}
```

- **`Menu`**, a plain owned value: `Menu.create()`; `addMenu(title)` starts a top-level menu; `addItem(id: u32,
  title, shortcut: MenuShortcut?)` adds an enabled, unchecked item to the last menu; `addSeparator()`. These three
  return `Result<void, AllocError>`, and copy the title, so `Menu` has no region parameter; a failed one leaves the
  menu as it was. `setEnabled(id, bool)` and `setChecked(id, bool)` change every item with that id, allocate nothing,
  and return nothing. There are **no submenus** (§5).
- **`MenuShortcut { key: WindowKey, modifiers: WindowModifiers }`**, ADR 0069's types. `Other` keys, `Function`
  outside 1 to 12 and a `Character` that is not a printable scalar show no shortcut.
- **`w.setMenu(menu: borrow Menu) -> Result<void, WindowError>`** shows the menus as the window's, replacing those it
  showed; an empty `Menu` removes them. The window keeps a copy: a change to the `Menu` shows when it is set again.
  `Unsupported` for a menu with an item or separator before its first `addMenu`, or with more than 4096 entries, and
  on X11; `Resources` when the system refuses. A failed `setMenu` leaves the window's menus as they were.
- **`w.nextMenuChoice() -> u32?`**, the id of the next item the user chose, or null; it never blocks. Choices are a
  window's, in order, apart from its events: no `WindowEvent` variant is added, so every exhaustive `match` on
  `WindowEvent` stays valid, as ADR 0142 kept it. A queued choice ends `waitEvent`, `waitFrame` and `Window.waitAny`,
  as an event does. A window keeps at most 64 choices not taken; a program that never takes them loses the newest.
- **macOS:** the application's main menu (`NSApp.mainMenu`). Its first menu is the application's: About *name*,
  Hide *name* (⌘H), Hide Others (⌥⌘H), Show All, Quit *name* (⌘Q), *name* the process's. Then the program's menus.
  - **One menu bar.** The last `setMenu`, of any window, wins, and its window receives the choices. When that window
    is dropped, its menus leave the bar and the application's menu stays.
  - **Quit** queues `Closed` in every window of the program (the program decides; the runtime never terminates it).
  - **Shortcuts.** A key down that is the shortcut of an enabled item is that item's choice, and gives no `Key`
    event, nor its key's release. A disabled item's shortcut is an ordinary `Key`. The menu is asked before the
    input method (text input on, ADR 0141), except while it composes, and before the runtime's ⌘Q and ⌘W, which
    queue `Closed` (§26.13), so a program's own ⌘W item takes ⌘W.
  - The items' enabled flags are the program's (`setAutoenablesItems:NO`), not AppKit's validation.
- **Windows:** the window's own menu bar (`CreateMenu`, `SetMenu`), destroyed with the window. An item's `WM_COMMAND`
  is its choice: its command is its index plus 1, since `WM_COMMAND` carries 16 bits and an id 32.
  - **Shortcuts are display-only**: the text after a tab, as "Ctrl+Shift+S" (Ctrl, Alt, Shift, then Win for
    `command`). The key reaches the program as a `Key`, which the program's own key handling acts on (§3).
  - `&` in a title is shown as itself (no mnemonics); Alt alone or F10 opens the menu bar, as in every Windows
    program with one.
  - The content keeps its size: the window grows by the menu bar (unless it is maximized), so `setMenu` gives no
    `Resized` unless the content's size did change.
- **X11:** `Unsupported`.

### 2.2 File dialogs (lib/std)

```ceesafe
match (FileDialog.openFile(mut window, "Open a picture", "png,jpg")?) {
    Some(path) => { /* open path */ }
    null => {}                                   // cancelled
}
```

- **`FileDialog`**, a struct of static functions. Each is modal: it returns when the user answers.
  - `openFile(window: borrow mut Window, title: StringView, extensions: StringView) -> Result<String?,
    WindowError>`: an existing file, or null for a cancel;
  - `openFiles(window, title, extensions) -> Result<Vector<String>, WindowError>`: one or more files; empty for a
    cancel;
  - `saveFile(window, title, suggestedName: StringView, extensions) -> Result<String?, WindowError>`: where to save,
    the name field starting as `suggestedName`; the system asks before an existing file is chosen, and the program
    writes the file;
  - `chooseFolder(window, title) -> Result<String?, WindowError>`: an existing folder.
- **Paths** are absolute, UTF-8 `String`s. A path the system gives that is not UTF-8 (possible on Linux, or an
  unpaired surrogate on Windows) is `Unsupported`.
- **`extensions`** is a comma-separated list of extensions without the dot ("png,jpg"; spaces and a leading dot are
  ignored), which the dialog offers alone; empty offers every file. A save dialog adds the first one to a name typed
  without one (macOS, Windows).
- **The window** owns the dialog (Windows' owner, X11's transient parent for `kdialog`), and on a scripted window
  answers it (§2.3). A `Window` is only ever on the program's first thread (§26.13), so these are too: their
  `NotMainThread` is not reached.
- **While a dialog runs** the program is not running: its windows are not repainted by the program, and the system
  shows their last frames (on X11 the runtime repaints them from the last frame and answers the clipboard). Input to
  the program's windows meanwhile: none on macOS (the panel is application-modal); on Windows, none to the owner,
  while the program's other windows queue theirs; on Linux the dialog is another program's window, not modal, and
  every window queues its input.
- **Effects:** `@effects(sleep, alloc)`: they wait for a person and allocate the paths.
- **macOS:** NSOpenPanel or NSSavePanel, `runModal` (app-modal); the title as the panel's title and message (panels
  without a title bar show the message); the extensions as `UTType`s (`setAllowedContentTypes:`, macOS 11, the
  UniformTypeIdentifiers framework loaded with `dlopen` as AppKit is), else as `setAllowedFileTypes:`. Folders:
  `canChooseDirectories`, new folders allowed.
- **Windows:** `IFileOpenDialog` and `IFileSaveDialog` through COM: `ole32.dll` loaded with `LoadLibraryW`, as ADR
  0141 loads `imm32.dll`, so no program needs a link flag; the class and interface ids written in the runtime, so no
  `uuid` library either. COM is initialized apartment-threaded for the call, and left as it was. Options:
  `FOS_FORCEFILESYSTEM`, `FOS_PATHMUSTEXIST`, and `FOS_FILEMUSTEXIST`, `FOS_ALLOWMULTISELECT`, `FOS_PICKFOLDERS` or
  `FOS_OVERWRITEPROMPT` by function; the extensions one filter, `*.png;*.jpg`. A cancel is
  `HRESULT_FROM_WIN32(ERROR_CANCELLED)`.
- **Linux:** `zenity --file-selection`, or `kdialog` (first when `XDG_CURRENT_DESKTOP` names KDE), found on `PATH`
  with `posix_spawnp`; neither there is `Unsupported`. Its standard output is the answer, a path a line, exit status 1
  a cancel, any other status `Resources`. Its input and error output are `/dev/null`. While it runs, the runtime polls
  its output every 50 ms and pumps every window.

### 2.3 The scripted window

- **`FRAME:menu:ID`**, ID a decimal `u32`: the user chooses the item ID at frame FRAME. The choice is queued then;
  `nextMenuChoice` hands it out only if the window's menu, when it gets there, has an enabled item ID, and drops it
  otherwise, as the system never delivers a choice of a missing or disabled item. So a test also checks the program's
  enabled flags.
- **`FRAME:dialog:PATH[|PATH…]`** and **`FRAME:dialog`** (no colon): from frame FRAME, the answer of the window's
  next file dialog: the paths (`|`-separated; a one-path dialog takes the first), or a cancel. Each dialog takes the
  earliest answer that is due, and cancels when there is none. The extensions and titles are not checked.
- `setMenu` is accepted and remembered (the copy the choices are checked against).

### 2.4 The runtime

- `ceesafe_window_set_menu(window, entries, length)`: lib/std's encoding of the `Menu`, per entry a 16-byte header
  (kind, flags, the shortcut's modifier bits, 0, then the id, the shortcut's key number and the title's length as
  little-endian `u32`s) and the title's bytes. The runtime checks it (every entry whole, a known kind, a menu first,
  at most 4096), keeps a copy and the items' ids in order, and calls the backend; on a failure it keeps the old ones.
- `ceesafe_window_menu_choice(window)`: the next id, or -1; with none queued it pumps first, as `poll` does.
- `ceesafe_window_dialog(window, kind, title, name, extensions, &answer)`: the answer's length, its paths each
  followed by a zero byte (kept in the window until its next dialog), 0 for a cancel, or minus a `WindowError`.
- **macOS:** a `CeeSafeMenuTarget` class (`choose:`, `quit:`) made at the first `setMenu`, whose one instance is
  every item's target; an item's tag is its id. An action runs on the main thread, inside a pump's `sendEvent:`, inside
  AppKit's run loop during a wait (a click through accessibility), or in `performKeyEquivalent:`; it queues the choice
  and posts the runtime's wake event (ADR 0142 §2.4), so a wait that AppKit's run loop was in returns.

## 3. Alternatives considered

- **A `WindowEvent.Menu(u32)` variant.** It would break every exhaustive `match` over `WindowEvent` in every program
  (ClayWind's included); a separate queue costs one more loop and breaks nothing, as ADR 0142's wake did.
- **Accelerator tables on Windows**, so that a shortcut is the item's choice there as on macOS. `TranslateAcceleratorW`
  would be needed in the pump for every message of a window with a menu, before the input method's (ADR 0141) and the
  program's keys; an accelerator cannot hold the Windows key, which is `command`, so the rule would have exceptions;
  and it could not be run on Windows for this ADR. A program must handle its shortcuts as keys on X11 anyway, which
  has no menu bar; with display-only text, Windows takes the same path, and only macOS, whose users expect the menu to
  take them, differs. Accelerators can be added later without changing the API.
- **Menus per window on macOS**, the bar switching with the key window. More faithful to Windows, but it moves the
  menu bar under the user whenever focus moves between a program's windows, and a program can do it by calling
  `setMenu` on a `Focus(true)`.
- **`Menu` holding a region for borrowed titles.** Titles are short and set rarely; owning them keeps `Menu` a plain
  value a program can store anywhere.
- **Native file dialogs on Linux through GTK or the XDG desktop portal** (D-Bus). Either needs a library or a D-Bus
  client in the runtime; `zenity` and `kdialog` are on the systems that have a desktop, and a program can ship its own
  dialog (ClayWind can draw one) where neither is.
- **Asynchronous dialogs** (macOS sheets, a callback or an event at the answer). A modal call is what every
  program's "Open…" needs, and keeps the API four functions; a sheet can be a later variant.

## 4. Consequences

- **New API:** `Menu`, `MenuShortcut`, `Window.setMenu`, `Window.nextMenuChoice`, `FileDialog` (`openFile`,
  `openFiles`, `saveFile`, `chooseFolder`). No new `WindowEvent` or `WindowError` variant.
- **macOS behaviour change only for a program that calls `setMenu`:** its keys are offered to the menu first, and ⌘Q
  is the application menu's Quit (every window's `Closed`, where before only the key window's).
- **Not covered:** submenus, mnemonics, icons and radio groups in menus; Windows accelerators (§3); a menu that a
  window shows without a menu bar (context menus); dialogs as sheets; the XDG portal; Wayland, as before.
- **Also fixed in §26.13:** macOS keeps at most 16 windows open at once (`CEE_MAC_WINDOWS`); the 17th `open` is
  `Resources`. That was true since ADR 0069 and not written.

## 5. The questions this ADR settles (the owner may revisit them)

1. **Submenus:** out of v1; one level of menus of items.
2. **Changing a menu:** `setEnabled` and `setChecked` change the `Menu` value; `setMenu` again shows the change. No
   handle to a live system menu.
3. **macOS's one bar:** the last `setMenu` wins, its window gets the choices, and when it is dropped only the
   application's menu is left. Not the key window's menu (§3).
4. **macOS's Quit:** `Closed` in every window, never a termination.
5. **Shortcuts:** macOS takes an enabled item's shortcut (a choice, no `Key`); Windows shows it as text only (§3).
6. **Ids:** any `u32`, chosen by the program; several items may share one (`setEnabled` changes them all).
7. **X11 menus:** `Unsupported`; the program draws its own.
8. **The scripted grammar:** `menu:ID` and `dialog[:PATH|…]`, in the script's `FRAME:WHAT` form; a scripted choice
   counts only for an enabled item of the window's menu.
9. **Dialogs' owner:** a `Window` argument, on the program's first thread by construction.
10. **Linux dialogs:** `zenity` or `kdialog` on `PATH`, else `Unsupported`; not modal.
11. **Limits:** 4096 entries in a menu, 64 choices queued in a window.

## 6. Implementation and evidence (2026-11-15)

**Files:**
- `lib/std/window.cee`: `Menu`, `MenuShortcut`, `Window.setMenu`, `Window.nextMenuChoice`, `FileDialog` (§2.1,
  §2.2).
- `runtime/hosted/ceesafe_rt_window.c`: §2.3 and §2.4, the three backends.
- `tests/reference/tests/corpus/check-pass/window-menu.cee`: a host that builds its menus, takes choices after each
  wait and runs every dialog; `run-pass/window-menu.cee`: a `Menu` built and changed without a window, natively and in
  the interpreter.
- `tools/window_check.py`: the scripted menu and dialogs (choices of enabled items only, each dialog function's
  answer and cancel, a choice ending a `waitEvent` and a `waitAny` of 20 s, a malformed menu, the interpreter); with
  `--display`, the real menu bar on macOS through System Events (its menus and items read, an item clicked, an enabled
  and a disabled item's shortcut typed, an open panel cancelled and another answered), Windows' `WM_COMMAND` (a
  package with a C helper), and on Linux a stand-in `zenity` first on `PATH`, and none.

**Tests** are recorded in ROADMAP §10 (2026-11-15, ADR 0146), with the commands and results on each system.
