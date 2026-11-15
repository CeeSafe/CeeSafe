# ADR 0141 — Text input through the input method, and the clipboard's text

- Status: **Accepted** by the owner, 2026-11-15. ClayWind's roadmap step 7, "IME, clipboard and selection", needs
  them from `Window`. The owner chose to add them to CeeSafe rather than to ClayWind's `extern "C"` layer ("Change
  CeeSafe too"). Implemented on branch `adr/0141-text-input-clipboard`.
- Scope: `lib/std/window.cee`, `runtime/hosted/ceesafe_rt_window.c`, type-system.md §26.13, `spec/api/std.md`
  (generated), `tools/window_check.py`, the corpus. No language, emitter or interpreter change.

## 1. Context

- **A window takes typed text one key at a time.** `WindowEvent.Text(u32)` is the character a key produced (ADR
  0069). An input method, which composes text over several keys and offers candidates (Japanese, Chinese, Korean; also
  macOS's dead keys and its accent menu), never reaches the program:
  - macOS: the runtime reads `characters` from each `NSEvent` and never calls `interpretKeyEvents:`, so the input
    method sees no keys;
  - Windows: `DefWindowProcW` handles the `WM_IME_*` messages. The system draws the composition in its own window at
    the window's corner, and only the result arrives, as `WM_CHAR`;
  - Linux: `XLookupString` has no input method at all.
- **A text field needs the composition itself.** It draws the text being composed inline, underlined, with its
  caret, and tells the input method where that is, so that the candidates appear next to it.
- **There is no clipboard.** A text field's copy, cut and paste need the system clipboard. Without it, ClayWind can
  copy only within one process.

## 2. Decision

### 2.1 The API (lib/std)

```ceesafe
window.setTextInput(true);                                     // a text field took focus
window.setTextInputArea(WindowBox { x: 120, y: 48, width: 2, height: 18 });  // the caret, in points
match (window.nextEvent()) {
    Some(WindowEvent.Composition) => {
        let text = window.composition();                       // "かな", empty when the composition ended
        let caret = window.compositionCaret();                 // a byte offset in it
        // draw it underlined at the caret
    }
    Some(WindowEvent.Text(c)) => { /* committed text, as before */ }
    ...
}
window.setClipboardText(selected)?;                            // Result<void, WindowError>
var pasted = String.create();
if (window.clipboardText(mut pasted)) { /* insert it */ }
```

- **`WindowEvent.Composition`** (no payload): the composition, or its caret, changed, or the composition ended.
  Committed text still arrives as `Text` events, one per scalar, so a program that ignores compositions still gets
  every character. A key that the input method takes gives no `Key` event, and neither does its release. Otherwise
  a text field would act on the Backspace or Enter that edits or commits a composition.
- **`setTextInput(on: bool)`** turns the input method on for the window, when a text field has focus, or off. Off
  ends a composition, with a `Composition` event. **Until the first call, a window behaves as before this ADR** (§2.3),
  so existing programs see no change.
- **`setTextInputArea(area: WindowBox)`**: where the caret or the composed text is, in points from the content's top
  left. `WindowBox`, which `presentChanged` uses in pixels, is reused rather than adding a fourth box type. The doc
  comment says which unit each use takes.
- **`composition() -> StringView`** and **`compositionCaret() -> usize`**: the text being composed, as UTF-8 (empty
  when there is none), and the caret's byte offset in it, always at a character's start.
  - **Ownership.** The view is of the window's own copy. That copy changes only when `nextEvent` returns
    `Composition`, which then makes it the latest composition. The view borrows the window (§25.6's elision), so the
    borrow checker rejects a `nextEvent`, or any other `borrow mut` call, while it is used (N0241). The corpus test
    `check-fail/window-composition-loan` shows it.
  - **Several changes between polls.** Each change queues an event, and each event shows the latest composition, so
    a program that redraws on each one is never behind.
- **`setClipboardText(text: StringView) -> Result<void, WindowError>`**: puts the text on the system clipboard.
  `Resources` when the system refuses.
- **`clipboardText(out: borrow mut String) -> bool`**: appends the clipboard's text to `out`. It is false when the
  clipboard holds no text, its text is not valid UTF-8, or memory ran out, and `out` is then unchanged. It is a
  method of `Window`: Windows' and X11's clipboards need a window, and X11's owner must answer requests while the
  program polls.

### 2.2 The runtime

- **Calls:**
  - `ceesafe_window_set_text_input(window, on)` and `ceesafe_window_set_text_area(window, x, y, w, h)`;
  - `ceesafe_window_composition(window, &text, &caret) -> u64`: the program's copy and its length;
  - `ceesafe_window_set_clipboard(window, text, length) -> i32`: 0, or minus a `WindowError`'s number;
  - `ceesafe_window_clipboard(window, &text) -> i64`: the length of the text the window now holds, or -1 for none.
    lib/std copies it into the `String` at once.
- **Two copies of the composition.** The backend's copy changes when the input method says so, and queues
  `COMPOSITION` (event kind 9) only when the text or the caret changed. `ceesafe_window_poll` copies it into the
  program's copy when it hands that event out. A callback that comes in another call, such as a Win32 message sent
  during `SetWindowPos`, can then never change the text under a live view.
- **Taking a key back.** A backend queues a key's `KEY` before it knows whether the input method takes the key. If it
  does, the backend sets that queued event's kind to 0, and `ceesafe_window_poll` skips such events. The order of
  `KEY`, `TEXT` and `COMPOSITION` is then the order in which they happened.
- **The text input area** is kept in the window, and each backend gives it to the input method (below).

### 2.3 The backends

- **macOS.**
  - **The content view.** It is now `CeeSafeContentView`, an `NSView` subclass registered through the Objective-C
    runtime. It adopts `NSTextInputClient` (`class_addProtocol`) and is the window's first responder, so
    `inputContext` gives it an input context. Its methods:
    - `insertText:replacementRange:`: `Text` events, then the end of the composition;
    - `setMarkedText:selectedRange:replacementRange:`: the composition, with the selection's UTF-16 start converted
      to a byte offset;
    - `unmarkText`: the composition is discarded, as winit and SDL do. Input methods commit with `insertText:`;
    - `hasMarkedText`, `markedRange`, `selectedRange`;
    - `firstRectForCharacterRange:actualRange:`: the text input area, converted to screen coordinates with
      `convertRectToScreen:`;
    - `validAttributesForMarkedText` (none), `attributedSubstringForProposedRange:actualRange:` (nil: the document is
      the program's), `characterIndexForPoint:` (`NSNotFound`), and `doCommandBySelector:`, which does nothing, so the
      key's `Key` event stands.
  - **Keys with text input on.** A key down goes to `interpretKeyEvents:`, unless ⌘ or Control is held without a
    composition: those stay shortcuts, as before. The key's `Key` event is taken back when there was a composition
    before the key, or when the key changed the composition. Its key up is then dropped too. The old `characters`
    path is not used, so ordinary typing gives one `Key` and one `Text`, as before. Off, `discardMarkedText` ends a
    composition.
  - **The clipboard.** `NSPasteboard.generalPasteboard`: `clearContents` and `setString:forType:`, then
    `stringForType:`, with `NSPasteboardTypeString`.
- **Windows.**
  - **imm32.** `imm32.dll` is loaded with `LoadLibraryW` at first use. MinGW ignores `#pragma comment(lib)`
    (ClayWind's finding J2), and with this no program needs a new link flag. `imm.h` supplies only types and
    constants.
  - **Text input on.** `ImmAssociateContextEx(hwnd, NULL, IACE_DEFAULT)`. The window then handles:
    - `WM_IME_SETCONTEXT`: `ISC_SHOWUICOMPOSITIONWINDOW` is cleared, so the system draws no composition;
    - `WM_IME_STARTCOMPOSITION`: places the input method's windows, without the default handling;
    - `WM_IME_COMPOSITION`: `GCS_RESULTSTR` as `Text` events, and `GCS_COMPSTR` and `GCS_CURSORPOS` as the
      composition (UTF-16 to UTF-8, the caret converted). `DefWindowProcW` is not called, so no `WM_IME_CHAR` or
      `WM_CHAR` repeats the result;
    - `WM_IME_ENDCOMPOSITION`: the end of the composition.

    A key the input method takes arrives as `VK_PROCESSKEY`: it gives no `Key`, and the release of its real key
    (from the scan code) none either.
  - **Text input off.** `ImmNotifyIME(…, NI_COMPOSITIONSTR, CPS_CANCEL, 0)` cancels a composition, then
    `ImmAssociateContextEx(hwnd, NULL, 0)` removes the input context, so keys reach the program as keys.
  - **The area.** `ImmSetCompositionWindow` (`CFS_POINT`) and `ImmSetCandidateWindow` (`CFS_EXCLUDE`, the area),
    in pixels.
  - **The clipboard.** `CF_UNICODETEXT`: `OpenClipboard(hwnd)` (a few tries: another program may hold it),
    `EmptyClipboard`, and `SetClipboardData` with a `GlobalAlloc(GMEM_MOVEABLE)` block, freed if the call fails. A
    read is `GetClipboardData`, bounded by `GlobalSize`.
- **Linux (X11).**
  - **The input method.** At the first `setTextInput(true)`, `XOpenIM` and `XCreateIC` (`XSetLocaleModifiers("")`).
    XOpenIM finds most input methods only in a locale other than C, so when `LC_CTYPE` is C or POSIX, the runtime
    sets it from the environment for the call and then restores it.
  - **Styles.** Over the spot (`XIMPreeditPosition` with `XNSpotLocation`, at the area's bottom left) where the input
    method offers it, else `XIMPreeditNothing`, else `XIMPreeditNone`. The input method draws its pre-edit itself, so
    **X11 gives no `Composition` events**. On-the-spot pre-edit (`XIMPreeditCallbacks`) is a larger protocol, left for
    later (§4).
  - **Events.** Every event goes through `XFilterEvent` once an input method is open. A filtered key press gives no
    `Key`, and its release none either. With text input on, a key press's text comes from `Xutf8LookupString`. A
    press with key code 0 is text that the input method committed: `Text` events, no `Key`. Text input off unfocuses
    the context (`XUnsetICFocus`, after `Xutf8ResetIC`), and keys go through `XLookupString` as before.
  - **The clipboard.** The `CLIPBOARD` selection as `UTF8_STRING`:
    - **Writing.** The window takes ownership (`XSetSelectionOwner`) and keeps a copy. The pump answers
      `SelectionRequest` for `TARGETS` and `UTF8_STRING` with `XChangeProperty` and a `SelectionNotify`, and drops
      its copy at `SelectionClear`. Text larger than one request can carry is `Resources`: there are no `INCR`
      transfers.
    - **Reading.** While the window owns the selection, a read gives its own copy. Otherwise the runtime asks the
      owner with `XConvertSelection` and waits up to one second for `SelectionNotify` (`XCheckTypedWindowEvent`,
      which leaves other events queued). An `INCR` reply counts as no text.
- **The scripted window.**
  - `FRAME:compose:C:TEXT` sets the composition to TEXT with the caret at byte C. It is delivered only while text
    input is on, as a system delivers nothing without an input method. C must be at most TEXT's length and at a
    character's start, or the script is malformed.
  - `FRAME:clipboard:TEXT` puts TEXT on the scripted clipboard, and `FRAME:clipboard`, without a colon, empties it,
    as when another program copies an image.
  - `setClipboardText` and `clipboardText` use the same scripted clipboard. Committed text is `type:TEXT`, as before.
- **The interpreter.** `open` is still `Unsupported`, so no other window call can be reached and none is modelled.

## 3. Alternatives considered

- **`Composition(StringView, usize)` as the event's payload.** It is more direct. But `WindowEvent` is a plain value
  that `nextEvent` returns without a loan, and an event that borrowed the window could not be held while handling the
  next one. A copy into an owned `String` per event would allocate in `nextEvent`, which has no effect today. The
  event says only that the composition changed, and the window owns the text.
- **The caret in characters, or in UTF-16 units.** A byte offset indexes the `StringView` directly, the way every
  other text position in lib/std does.
- **Text input always on.** It would change every existing program on macOS and X11, and on Windows replace the
  system's composition window that existing programs rely on with nothing.
- **A separate `Clipboard` type.** Windows and X11 need a window, and X11's owner must keep answering requests.
  ClayWind has one window, so a method of it is the simpler shape.
- **On-the-spot pre-edit on X11** (`XIMPreeditCallbacks`). It would give `Composition` events on Linux too, but it
  is a protocol of four callbacks with their own text and feedback structures. The over-the-spot style is
  acceptable for the step (the owner's brief), and the API does not change when it is added.

## 4. Consequences

- **A new `WindowEvent` variant.** Every exhaustive `match` over `WindowEvent` needs a `Composition` arm (N0256).
  ClayWind's desktop hosts and `tools/window_check.py` are such matches.
- **New calls on `Window`:** `setTextInput`, `setTextInputArea`, `composition`, `compositionCaret`,
  `setClipboardText` and `clipboardText`. No new types.
- **Effects.** `setTextInput`, `setClipboardText` and `clipboardText` may sleep and allocate (X11 opens the input
  method and waits for the selection; Windows retries `OpenClipboard`). `setTextInputArea`, `composition` and
  `compositionCaret` have none.
- **Not covered:**
  - on-the-spot composition on X11, and text larger than one X request (INCR);
  - reconversion (`attributedSubstringForProposedRange:` with the document's text, `WM_IME_REQUEST`) and
    `replacementRange`, which need the program's document;
  - the clipboard's formats other than text, and X11's `PRIMARY` selection;
  - Wayland, as before.
- **The spec** is type-system.md §26.13.

## 5. Implementation and evidence (2026-11-15)

**Files:**
- `lib/std/window.cee`: `WindowEvent.Composition` and the six methods (§2.1).
- `runtime/hosted/ceesafe_rt_window.c`: §2.2 and §2.3. The scripted window's `compose` and `clipboard` items.
- `tests/reference/tests/corpus/check-fail/window-composition-loan.cee`: a composition view held across `nextEvent`
  or `setTextInput` is N0241; one copied into a `String` first is not.
- `tools/window_check.py`, type-system.md §26.13 and `spec/api/std.md`.

**Tests** are recorded in ROADMAP §10 (2026-11-15, ADR 0141), with the commands and results on macOS arm64, Linux
arm64 (a Debian 12 container with Xvfb) and Windows.
