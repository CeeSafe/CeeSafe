# ADR 0150 — Files dropped on a window

- Status: **Accepted** by the owner, 2026-11-15. ClayWind's roadmap step P3.6 is "drag and drop". A drag inside a
  window is ClayWind's own work, but files dragged from another program (Finder, Explorer, a file manager) reach a
  program only through the system, and `Window` (type-system.md §26.13) takes none. The owner said "Continue to step
  6"; the design below was the lead's, chosen as defaults, and the questions it settles are listed in §5 so that the
  owner can revisit them. Implemented on branch `adr/0150-file-drops`, released later.
- Scope: `lib/std/window.cee`, `runtime/hosted/ceesafe_rt_window.c`, type-system.md §26.13, `spec/api/std.md`
  (generated), `tools/window_check.py`, the corpus. No language, emitter or interpreter change.

## 1. Context

- **What a person does.** They select files in the system's file manager, drag them over a program's window, which
  shows that it would take them (the program highlights a drop zone, the system's cursor shows a copy), and release.
  The program then opens, imports or attaches the files. Zed's GPUI reports the same: a drag of paths entering,
  moving, leaving and dropping.
- **The systems.**
  - macOS: a view registers for pasteboard types (`registerForDraggedTypes:`) and is an `NSDraggingDestination`
    (`draggingEntered:`, `draggingUpdated:`, `draggingExited:`, `performDragOperation:`). The drag's pasteboard
    holds the files' URLs from the first callback, so the target knows them while the drag hovers.
  - Windows: `DragAcceptFiles` and `WM_DROPFILES` give a drop only, with no hover; an `IDropTarget` registered with
    OLE (`RegisterDragDrop`) gets the drag's entering, moves, leaving and drop, with the data object (`CF_HDROP`) from
    the first.
  - X11: the XDND protocol between two programs: the source sends client messages (`XdndEnter`, `XdndPosition`,
    `XdndLeave`, `XdndDrop`), the target answers each position (`XdndStatus`) and the drop (`XdndFinished`), and the
    data is the `XdndSelection` converted to a type, `text/uri-list` for files, which the target may ask for at any
    position as well as at the drop.
- **ADR 0146's pattern.** Menu choices are a queue beside the events, so no `WindowEvent` variant was added, and
  every exhaustive `match` on `WindowEvent` (ClayWind's among them) stays valid. Drops take the same path.

## 2. Decision

### 2.1 The API (lib/std)

```ceesafe
loop {
    window.waitEvent(1000);
    // nextEvent, nextMenuChoice, then:
    loop {
        match (window.nextDrop()) {
            Some(dropped) => { /* open dropped.paths, dropped at (dropped.x, dropped.y) */ }
            null => { break; }
        }
    }
    highlight(window.dragOver());   // WindowDragOver? — redraw the drop zone
}
```

- **`WindowDrop { x: f32, y: f32, paths: Vector<String> }`**: where the files were dropped, in points from the
  content's top left as pointer events are, and their paths, absolute and UTF-8, in the system's order, at least one.
- **`WindowDragOver { x: f32, y: f32, count: u32 }`**: where a drag of files is, and how many files it holds.
- **`w.nextDrop() -> WindowDrop?`** (`borrow mut`): the next drop, in order, or null; it never blocks. With none
  queued it takes the system's events first, as `nextEvent` and `nextMenuChoice` do. A queued drop ends `waitFrame`,
  `waitEvent` and `Window.waitAny`, as an event does. A window keeps at most 64 drops not taken; a program that never
  takes them loses the newest.
  - **Paths that are not UTF-8** (possible on Linux, and an unpaired surrogate on Windows) are left out, as is a path
    that memory ran out for; a drop with no path left is skipped. A file dialog's such path is `Unsupported` (ADR
    0146), but a drop is not a call that can fail: leaving one file out of several is the useful answer, and the
    count the hover gave says that some were.
- **`w.dragOver() -> WindowDragOver?`** (`borrow`): while a drag of files is over the window, where and how many, else
  null. It gives **no paths** before the drop (§5). It is the window's state as of the last system event the window
  took, and it allocates nothing.
  - **A change is a wake** (ADR 0142), not an event: the drag entering, moving or leaving ends the current
    `waitFrame`, `waitEvent` or `waitAny`, or the next one at once, and a wait's return takes it. A program redraws
    its highlight from `dragOver()` after each wait. No history of positions is queued: a program wants where the drag
    is now.
  - **A drop ends the drag**: after it `dragOver` is null.
- **Only files.** A drag that holds no file (text, an image's data, a URL that is not a file) is refused: the window
  answers the system with no operation, so the system shows no drop and the source's drag slides back. The window
  answers with the copy operation, or where the source allows none, a link (macOS: generic, then link); a source that
  allows only a move is refused, since the program does not move the files.
- **Effects:** `nextDrop` allocates (the paths); `dragOver` has none. Both run on the program's first thread, as
  every `Window` does.

### 2.2 The backends

- **macOS.** `CeeSafeContentView` (ADR 0141's class) registers for `NSPasteboardTypeFileURL` at each open (dropped
  with `unregisterDraggedTypes` at close), and gains the `NSDraggingDestination` methods, added with
  `class_addMethod` as its text input methods are:
  - `draggingEntered:` counts the pasteboard's file URLs (`readObjectsForClasses:` with `NSURL` and
    `NSPasteboardURLReadingFileURLsOnlyKey`), and with `draggingUpdated:` sets the drag at `draggingLocation`;
    `wantsPeriodicDraggingUpdates` is NO, so updates come with moves;
  - `draggingExited:` and `draggingEnded:` end the drag; `prepareForDragOperation:` takes only a drag of files;
  - `performDragOperation:` reads the URLs again and queues the drop: each URL's `filePathURL`'s `path` (a file
    reference URL stands for its path). File promises (`NSFilePromiseReceiver`: a mail's attachment, a photo
    library's picture) are not file URLs and are refused.
  - Each runs on the main thread inside AppKit's run loop, a pump's or a wait's; after a change it posts the runtime's
    wake event (ADR 0142 §2.4), so a wait that AppKit's run loop was in returns.
- **Windows: an `IDropTarget`.** Hover is half of the feature (ClayWind highlights where a drop would land), and
  `WM_DROPFILES` has none; so each window registers an `IDropTarget` of the runtime's own (a vtable in C, the window
  owning it, so its references are not counted).
  - `ole32.dll` is loaded with `LoadLibraryW`, as ADR 0146 loads it for the dialogs, so no program needs a link flag;
    `IID_IUnknown` and `IID_IDropTarget` are written in the runtime. `OleInitialize` runs at each window's open and
    `OleUninitialize` at its close (they count), `RegisterDragDrop` after the window is made and `RevokeDragDrop`
    before it is destroyed.
  - A thread whose COM is multithreaded cannot have OLE (`RPC_E_CHANGED_MODE`): that window takes no drops, and
    everything else works. ADR 0146's dialogs initialize COM apartment-threaded for the call, which on a window's
    thread is then OLE's already (`S_FALSE`, balanced as before).
  - `DragEnter` counts `CF_HDROP`'s files (`DROPFILES`, read from the `HGLOBAL`, UTF-16 or the ANSI code page);
    `DragOver` and `DragLeave` move and end the drag; `Drop` queues the paths, UTF-16 converted with
    `WC_ERR_INVALID_CHARS` (an unpaired surrogate is left out). The positions are screen pixels, made the content's
    points with `ScreenToClient` and the window's scale. OLE's target is the whole window, its frame included; over
    the title bar or the border the window answers `DROPEFFECT_NONE` and has no drag, since macOS's view and X11's
    window are the content alone (found on the laptop: a drag crossing the title bar reported a negative `y`). OLE
    calls the methods while the thread dispatches messages, that is in a pump, so they queue as the window procedure
    does, and a wait ends at the message as before.
- **X11: XDND version 5** (Linux, and FreeBSD with Xorg, whose backend is the same code: ADR 0149). At open the
  window's `XdndAware` is 5.
  - `XdndEnter`: the source's types, three in the message or more in its `XdndTypeList`; the drag is the window's to
    take only if `text/uri-list` is one.
  - `XdndPosition`: the root coordinates made the window's with `XTranslateCoordinates`. At the first, the window
    converts `XdndSelection` to `text/uri-list` (the protocol allows it at any position, with the position's time),
    and once the source answers, it knows the count: the drag then shows in `dragOver`, and the `XdndStatus` of later
    positions accepts only a list with a local file (until the answer, it accepts a drag that offers the type). Each
    position is answered with `XdndStatus`, the action `XdndActionCopy`.
  - `XdndLeave` ends the drag; `XdndDrop` ends it and converts again, at the drop's time; the `SelectionNotify`
    queues the drop and sends `XdndFinished` (accepted, `XdndActionCopy`, or not accepted with no file).
  - **The list:** lines separated by CRLF (or LF), `#` lines skipped; a `file:` URI of this host (`file:///path`,
    `file://localhost/path`, `file://HOST/path` with `gethostname`'s name, or `file:/path`), up to a `?` or `#`,
    percent-decoded. Another host's file, a URI of another scheme, a malformed `%` escape and a path holding a zero
    byte are not files. There are no `INCR` transfers (as with the clipboard, ADR 0141): a list longer than one
    request gives no files.
  - The clipboard's read (ADR 0141) waits for its own `SelectionNotify` and hands an XDND one it meets to the drop.
- **The scripted window** (§26.13's grammar, ADR 0069 §2.3):
  - `FRAME:dragover:X,Y:N` (N ≥ 1): a drag of N files is over the window at X,Y;
  - `FRAME:dragleave`: it left;
  - `FRAME:drop:X,Y:PATH|PATH…`: a drop, which ends the drag. The paths are the script's bytes, so a path that is not
    UTF-8 tests that `nextDrop` leaves it out. A malformed item is a malformed script (`open` is `Resources`).

### 2.3 The runtime

- `ceesafe_window_drop(window, &position, &paths)`: the next drop's paths' length (each path followed by a zero byte,
  kept in the window until the next call), its position, or -1; with none queued it pumps first.
- `ceesafe_window_drag_over(window, &position)`: the drag's count, 0 for none, and its position.
- Shared: `cee_window_drag` sets the drag and its `drag_changed` flag, which `waitEvent`, `waitFrame` and `waitAny`
  test with their events and clear when they return, as they take the wake; `cee_window_dropped` ends the drag and
  queues the drop in a ring of 64.

## 3. Alternatives considered

- **`WindowEvent` variants** (`DragEntered`, `DragMoved`, `DragLeft`, `Dropped(paths)`). They would break every
  exhaustive `match` over `WindowEvent` in every program, as ADRs 0142 and 0146 noted; and a variant cannot own a
  `Vector<String>` without making every `WindowEvent` a move type.
- **The hover as queued events** rather than a state and a wake. A program draws the drag where it is now; a queue of
  positions would make each program skip all but the last, and could fill the event queue during a long drag.
- **Paths during the hover.** macOS and Windows have them; X11 has them only after a conversion, which the source may
  answer slowly. A program decides where a drop lands by position, and the files at the drop; the count is enough to
  say "3 files". Paths can be added later as a call (`dragPaths`) without changing these.
- **`DragAcceptFiles`/`WM_DROPFILES` on Windows**: simpler (shell32, no COM), but no hover, so ClayWind could not
  highlight a drop zone there; and it accepts every drag of files with no say. OLE's thread rule (§2.2) is the cost.
- **Accepting text and URLs** (a dragged link, a selection of text). Each is a different value (`String`, a URL), and
  ClayWind's own drags inside a window cover text; files were the step's need. A later ADR can add them as more
  queues or a variant of `WindowDrop` without changing this one.
- **Refusing a drop over part of the window** (the system's "no" cursor outside a drop zone). It needs the program to
  answer during the drag, synchronously, from the system's callback; the program learns the position after a wait.
  A program ignores a drop outside its zones instead.

## 4. Consequences

- **New API:** `WindowDrop`, `WindowDragOver`, `Window.nextDrop`, `Window.dragOver`. No new `WindowEvent` or
  `WindowError` variant.
- **Behaviour change for every program with a window:** files dragged over it are accepted (the system shows a copy
  cursor), where before the system showed no drop; a program that never calls `nextDrop` loses them, as it would
  lose events it never takes. On Windows the program's first thread has OLE initialized while a window is open.
- **Not covered:** text, URLs and file promises; a program as a drag source (dragging out of a window); per-region
  refusal; paths before the drop; `INCR` transfers on X11; Wayland, as before.
- **Also fixed:** ADR 0146's Linux file dialog passed its argument list to `posix_spawnp` with a cast that drops
  `const`, which `-Wcast-qual`, part of every build's warning set, reported for every Linux program with a window; it
  now goes through `uintptr_t`, as the emitter's own conversions do (c-backend.md §2).

## 5. The questions this ADR settles (the owner may revisit them)

1. **A queue beside the events**, `nextDrop`, not a `WindowEvent` variant.
2. **The hover's content:** position and count, no paths (§3); on X11 the count is learned by converting the
   selection at the first position, so the drag shows once the source has answered.
3. **The hover as a state with a wake**, not queued events.
4. **Only files:** text, URLs and file promises are refused, and the system shows no drop for them.
5. **The operation:** copy, else generic or link; a move-only source is refused.
6. **Windows:** an `IDropTarget` with OLE initialized per window (hover), not `WM_DROPFILES` (no hover); a thread
   whose COM is multithreaded takes no drops.
7. **Non-UTF-8 paths** are left out of a drop (a drop left empty is skipped), where a file dialog's are an error.
8. **X11's list:** only this host's `file:` URIs; no `INCR`.
9. **The scripted grammar:** `dragover:X,Y:N`, `dragleave` and `drop:X,Y:PATH|…`.
10. **Limits:** 64 drops queued in a window.

## 6. Implementation and evidence (2026-11-15)

**Files:**
- `lib/std/window.cee`: `WindowDrop`, `WindowDragOver`, `Window.nextDrop`, `Window.dragOver` (§2.1).
- `runtime/hosted/ceesafe_rt_window.c`: §2.2 and §2.3, the three backends and the scripted window; the dialog's
  `-Wcast-qual` fix (§4).
- `tests/reference/tests/corpus/check-pass/window-drop.cee`: a host that highlights the drag and opens the drops
  after each wait; `run-pass/window-drop.cee`: the two values built without a window, natively and in the
  interpreter.
- `tools/window_check.py`: the scripted drags and drops (the trace of a script with paths that are not UTF-8, a drag
  change alone and a drop alone ending a `waitEvent` and a `waitAny` of 20 s, four malformed items, the
  interpreter); with `--display`, a person's drags made by a helper the checker builds with the C compiler: on macOS
  an Objective-C program whose window starts a dragging session of files (or text) and moves the pointer with the
  system's mouse events (`CGEventPost`) onto the program's window; on Windows a C program whose window starts an OLE
  drag (`DoDragDrop`) of `CF_HDROP` (or `CF_UNICODETEXT`), moved by `SendInput`, which first puts the target window on
  top, away from the consoles (a drag that crossed a console window was taken by the console); on X11 an XDND
  source in C. In each, a drag of text is refused, a drag of two files that enters and leaves shows in `dragOver`
  with its count and then not, and a dropped one arrives with both paths at the point of release, the source told so.
  The checker also names the last command it ran if it fails itself, and a command killed after 120 s is its case's
  problem.

**Tests** are recorded in ROADMAP §10 (2026-11-15, ADR 0150), with the commands and results on each system.
