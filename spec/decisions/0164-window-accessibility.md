# ADR 0164 — What a window holds, told to a screen reader

- Status: **Accepted** by the owner, 2026-11-17 ("I  accept ADR 0164 as written"), for ClayWind's roadmap phase P5
  (platform completeness; the owner: "Continue to P5"), whose gate is "the component library passes VoiceOver, NVDA
  and TalkBack checks". The API and macOS are implemented (§6); iOS, Android, Windows and Linux are the steps left.
  ClayWind's side exists (its commit `3b23725`): a frame's elements with role, name, value, state and place, and a
  screen reader's actions as the controls see them.
- Scope: `lib/std/window.cee` (`WindowAccessRole`, `WindowAccessNode`, `WindowAccessAction`, four methods of
  `Window`), `runtime/hosted/ceesafe_rt_window.c`, `runtime/android/` (a node provider and its embedded dex),
  type-system.md §26.13, `spec/api/std.md` (generated), `tools/window_check.py`. No compiler change, no change to the
  language.

## 1. Context

- **A CeeSafe window is a frame of pixels** (ADR 0069): the system sees one view with nothing in it. A screen reader
  says "window" and no more; a switch or a voice control has nothing to act on. ClayWind's finding I7.
- **Every system has the same model**, under five names: a tree of elements, each with a role, a name, a value, a
  state, a rectangle on the screen and a few actions; the app answers the reader's questions about it and tells it
  when something changed.

  | System | The API | The reader to pass |
  |---|---|---|
  | macOS | NSAccessibility (`NSAccessibilityElement`) | VoiceOver |
  | iOS | UIAccessibility (`UIAccessibilityElement`) | VoiceOver |
  | Android | `AccessibilityNodeProvider`, virtual views | TalkBack |
  | Windows | UI Automation providers (`IRawElementProviderSimple`, `…Fragment`, `…FragmentRoot`) | NVDA, Narrator |
  | Linux, FreeBSD | AT-SPI 2 over D-Bus | Orca |

- **A program that draws its own controls knows all of that** each time it draws: an immediate-mode library builds
  the frame's elements anyway. What is missing is a way to hand the list to the system and to hear what the reader
  asks.
- **Readers move through leaves.** VoiceOver, TalkBack and NVDA step from control to control in reading order;
  containers matter for tables and outlines, which no CeeSafe UI library has yet. A flat list in reading order is
  what the gate needs.

## 2. Decision

```
window.setAccessNodes(nodes[..], text[..]);       // the whole list, when it changed
match (window.takeAccessAction()) {               // what the reader asked, one at a time
    Some(a) => { /* a.id, a.action */ }
    null => {}
}
```

### 2.1 The types

- **`WindowAccessRole`**: `Text`, `Button`, `Checkbox`, `Radio`, `TextField`, `Slider`, `Tab`, `ListItem`,
  `MenuItem`, `Dropdown`, `Image`. Each has one counterpart on every system (§4); a role a system lacks is its
  nearest (a `Dropdown` is a pop-up button on macOS, a combo box under UI Automation).
- **`WindowAccessNode`**, a Copy struct: `id: u64` (the program's, the same from one list to the next for the same
  element, never 0); `role`; `flags: u32` (`checked`, `selected`, `focused`, `disabled`, `expanded`, `secure`, as
  constants of the struct); `x`, `y`, `width`, `height: f32` in points from the content's top left, as pointer events
  are; `min`, `max`, `now: f64` for a slider; `labelStart`, `labelLength`, `valueStart`, `valueLength: u32`, ranges
  of the `text` given with the list (UTF-8); and `caret` and `anchor: u32`, a text field's caret and the other end
  of its selection as byte offsets into its value (added after VoiceOver showed the need, §6; this replaces §2.3's
  "a later addition"). And `layer: u32`, which layer the element is drawn in (added in §10).
- **`WindowAccessAction`**: `Press`, `Focus`, `Increment`, `Decrement`. **`WindowAccessRequest`**: `id: u64`,
  `action`.

### 2.2 The methods

- **`w.setAccessNodes(nodes: Slice<WindowAccessNode>, text: Slice<u8>)`** (`borrow mut`): the window's elements, in
  reading order, replacing the last list whole. The runtime copies both, compares the list with the last by `id`,
  and tells the system what was added, removed, renamed, moved, or changed value, state or focus. An empty list is
  a window with nothing in it. A range outside `text`, an `id` of 0 or one given twice is a panic, as an index out
  of bounds is.
- **`w.takeAccessAction() -> WindowAccessRequest?`** (`borrow mut`): the oldest request not yet taken, or null. A
  request is queued when the reader asks it and the element is in the current list; the window's wait ends (ADR
  0142) so a program that sleeps between events sees it. The program does it, or not: the runtime changes nothing.
- **`w.accessActive() -> bool`**: whether something has asked the system for this window's elements since it
  opened (a reader, a switch, an automation tool). A program may leave `setAccessNodes` out while it is false, and
  must call it once it is true. It does not go back to false.
- **`w.announce(text: StringView)`**: says `text` through the reader now, without moving its cursor (a message
  arrived, a save finished). Nothing happens when no reader runs.
- All four are no-ops in `CEESAFE_WINDOW_SCRIPT` runs but for the script's own lines (§5), on a system whose part
  is not implemented yet (§3), and in the interpreter, where `takeAccessAction` is null and `accessActive` false.

### 2.3 What the runtime does not do

- **No tree.** The list is flat. Containers, tables and outlines are a later ADR, which adds a parent to a node
  without changing what is here.
- **No text editing protocol.** A `TextField` gives its value; a reader reads it, and typing goes through the
  keyboard as it does now (ADRs 0141, 0157). Caret and selection for readers (reading by character and word in a
  field) are a later addition to `WindowAccessNode`.
- **No drawing.** The reader draws its own cursor from the rectangle.

## 3. Order of work

Each system is its own step and its own release, in this order; the API is released with the first.

1. **macOS**: elements made through the Objective-C runtime in C, as the rest of the Cocoa part is; the content
   view answers `accessibilityChildren` and `accessibilityHitTest:`. Checked with the Accessibility Inspector's
   API (`AXUIElement`) by `window_check`, and by the owner with VoiceOver.
2. **iOS**: `UIAccessibilityElement`s as the view's `accessibilityElements`. The owner with VoiceOver on the phone.
3. **Android**: an `AccessibilityNodeProvider` in `org.ceesafe.window`, virtual views by id. The owner with TalkBack.
4. **Windows**: UI Automation providers written as C vtables, answering `WM_GETOBJECT`. The owner with NVDA.
5. **Linux and FreeBSD**: AT-SPI 2 on the accessibility bus through `libdbus-1.so.3`, loaded with `dlopen` as libX11
   is. Checked in a container with `python3-pyatspi`; Orca by whoever has such a desktop.

## 4. Alternatives considered

- **A retained tree with handles** (`createNode`, `setLabel`, `removeNode`). It is what the systems have underneath,
  and what a retained UI library would want. An immediate-mode one would have to diff its frame against handles
  itself; the runtime can do that once, by id.
- **Events as a `WindowEvent` variant.** A new variant breaks every exhaustive `match` of the enum in programs that
  exist. Drops and touches are taken by their own methods for the same reason (ADRs 0150, 0155).
- **AccessKit**, the Rust library that maps one tree to all five. It is the model for this ADR's shape. Using it
  would add a Rust toolchain and a prebuilt library per target to a runtime that is C with no dependencies.
- **Only the two mobile systems, or only the desktops.** The gate names VoiceOver, NVDA and TalkBack, which are
  macOS or iOS, Windows, and Android.

## 5. What an implementation must show

- For each system as its step is done: the list as the system reports it back (role, name, value, state, frame) for
  a window with one element of each role; an action asked through the system's API arriving in `takeAccessAction`;
  a changed value and a moved focus reaching a listener; `accessActive` false before any question and true after.
- `CEESAFE_WINDOW_SCRIPT`: a line `access <id> <action>` queues a request, and the list given to `setAccessNodes`
  is written to the script's log, so `window_check` covers the API on every system without a reader.
- The owner's checks with VoiceOver (macOS, iOS), TalkBack and NVDA on ClayWind's component demo: every control is
  reached in order, says its role, name, value and state, and can be activated.
- The corpus, the contract suite, the release chain; that a program which never calls these is unchanged.

## 6. Step 1 as implemented: the API and macOS (2026-11-17)

`lib/std/window.cee`, `runtime/hosted/ceesafe_rt_window.c`, type-system.md §26.13, `tools/window_check.py` item 15.

- **As §2 has it**, with these made exact: a window keeps 16 requests not taken and drops a newer one; a request
  of a `disabled` element is not queued (macOS answers the reader that the action failed); out of memory, the last
  list stays.
- **One change from §5: there is no script log.** A scripted run has no log to write the list to, and adding one
  for this would be a second way to do what the test program does by printing. `access:ID,ACTION` queues a request
  as §5 says, and the check's program prints what it takes. What the system is told is checked on the system
  itself, below.
- **A scripted window's frame 0 is taken when the window opens**, before a program can have given a list, so a
  request there finds no element: scripts ask from frame 1.
- **macOS, checked in the process**: `window_check --display` builds a program with one element of each role and a
  C file that asks AppKit for the content view's `accessibilityChildren`, which needs no permission as another
  process's questions do. AppKit reports the eleven roles with their names, values, states and frames (to the
  point); `accessibilityPerformPress`, `…Increment`, `…Decrement` and `setAccessibilityFocused:` arrive from
  `takeAccessAction` in order; a press of the disabled element does not; after a second list with a changed value
  and focus, all eleven elements are the same objects and say the new value and focus; `accessActive` is false
  before the first question and true after. 76 checks, 0 problems.
- **VoiceOver, 2026-11-17**, on ClayWind's component demo, driven with VoiceOver's keys and read from its
  caption panel (the owner: "Turn VoiceOver back on and finish the check"). What it says: "Clicked 0, button";
  "Reset, dimmed, button"; "Dashboard, selected, tab"; "Subscribe to updates, unchecked, checkbox"; "Dark,
  selected, radio button"; "High, Priority, collapsed, pop up button"; "50%, Volume, slider"; "Al, Name, text";
  plain text as it is written. Its activate command clicks a button, and it says the button's new name by itself.
  Four things it found, each fixed:
  - **The order was the screen's, not the list's**: with the view no element itself, VoiceOver read a sidebar's
    rows between a page's. The view is now a group whose children in navigation order are the list.
  - **A count was a percentage** ("3%, Quantity, slider" for 3 of 99): a slider's value text, when the node has
    one, is its value's description.
  - **A text field said its name twice and never its text**: a field's element now answers for its text.
  - **The list was one action stale** (two radio buttons chosen, a dropdown still open): ClayWind's, not the
    runtime's. A reader's action is input to its Ui, so a second frame says what the action did.
- **The storage of the list changed after the first commit**: a C-layout struct in `lib/std` for it appeared in
  every program's exports header (20 `ffi` tests and 29 `emit-c` snapshots differed). The list crosses as nine
  64-bit words a node.
- **The open points, closed the same day** (the owner: "Go ahead and fix the open points"):
  - **An empty field said its name twice.** It has no value now, and says "Note, edit text".
  - **Reading by character.** `WindowAccessNode` has `caret` and `anchor` (§2.1); a field's element answers its
    selection from them, says its value and selection can be set, and `AXSelectedTextChanged` is posted when they
    change. VoiceOver then calls the field "edit text" and says each character the caret passes: with "Hello" in
    the field and the Left arrow pressed four times, "o", "l", "l", "e". The caret is still moved by the keyboard
    through the program; a reader setting the selection itself is not taken.
  - **VoiceOver's own adjustment of a slider** (interact, then its arrows): "In slider", "60%", "70%", "60%", "Out
    of slider", and the program's value followed.
  - **The pages not walked before**: a list's rows ("README.md, selected, button", then each row), a row of
    buttons ("1 M, button"), and the demo's other pages, which are text. On its Drag page the cards are read as
    text: dragging is not something this API gives a reader.
  - `window_check --display` also reads the field's selection back from AppKit (caret 1, anchor 2 of "Al":
    `1+1`, "l").
- **Open**: a reader setting a field's selection or value itself; dragging; containers and tables (§2.3); the
  owner's own pass with VoiceOver.

## 7. Step 2 as implemented: iOS (2026-11-18)

`runtime/hosted/ceesafe_rt_window.c` (the UIKit part), type-system.md §26.13. The owner: "continue".

- Each node is a `UIAccessibilityElement` in the window's view; the view's `accessibilityElements` are the list.
  iOS has traits and no roles, so several roles are one thing to VoiceOver (type-system.md §26.13 has the table):
  a tab, a radio button, a list's row and a menu's item are buttons, selected when they are.
- **`accessActive` is whether VoiceOver runs**, not whether something asked: UIKit does not say when it reads the
  elements.
- **`Focus` is never asked**: VoiceOver's own cursor is not the keyboard's, and activating a field is a `Press`.
- **A field's caret and anchor are not used**: reading by character in a field needs `UITextInput` on the element,
  which is not here.
- **Checked in the iOS Simulator, in the process**: a program with one element of each role and a C file that asks
  UIKit for the view's `accessibilityElements`. UIKit reports the eleven names, values, traits and frames;
  `accessibilityActivate`, `…Increment` and `…Decrement` arrive from `takeAccessAction`; the disabled element
  refuses; after a second list all eleven are the same objects and the slider says its new value. It is not in
  `window_check`, which builds for the system it runs on.
- **Not checked yet**: VoiceOver on a phone, which the Simulator does not have.

## 8. Step 3 as implemented: Android (2026-11-18)

`runtime/android/org/ceesafe/window/AccessView.java` and its dex, `runtime/hosted/ceesafe_rt_window.c` (the Android
part), type-system.md §26.13. The owner: "Moto is connected. Go ahead install and perform tests on it."

- A view the size of the window with an `AccessibilityNodeProvider`: the list's elements are its virtual views.
  The list crosses from the program's thread to the activity's packed in bytes; what TalkBack asks comes back
  under the runtime's lock and is taken by the program's next poll.
- **`Focus` is never asked**, as on iOS; **a field's caret and anchor are not used**.
- **TalkBack's focus rectangle is not drawn**: a NativeActivity's window surface is the program's, so the view
  system that paints the rectangle paints nothing. What is spoken and what is done are not affected. A program
  could draw one if the runtime told it where TalkBack's cursor is, which it does not.
- **Checked on a moto g35 5G, Android 15, TalkBack 17**:
  - `uiautomator dump` (the system's own reading of the node tree): the component demo's 75 elements of its first
    screen with their widget, name, selected state, clickability and bounds.
  - TalkBack reads the list and puts its cursor on the first element when it starts.
  - **Touches injected with `adb` go round TalkBack**, so its gestures cannot be scripted. The owner made them, with
    the view's requests logged for the session: a finger exploring the screen moved TalkBack's cursor from element
    to element (180 hover events, the cursor on ten elements in turn), and a double tap activated one. The owner:
    "It's working fine without issues."
- **Not checked**: a slider's steps through TalkBack; `announce`; a second activity after the first ends.

## 9. Step 4 as implemented: Windows (2026-11-18)

`runtime/hosted/ceesafe_rt_window.c` (the Win32 part), type-system.md §26.13. The owner: "Do Windows and release
all together".

- UI Automation providers in C: a fragment root for the window, answering `WM_GETOBJECT`, and a provider for each
  element, with the pattern its role has. The interfaces are their vtables written out, and nothing is linked:
  `uiautomationcore.dll` and `oleaut32.dll` are loaded at first use.
- **A slider is set by steps**: `IRangeValueProvider::SetValue` above the value asks `Increment`, below it
  `Decrement`. **A field's value cannot be set by a client**; `SetFocus` on it asks `Focus`, and the keyboard types.
- **Checked on Windows 11 with a UI Automation client** (`System.Windows.Automation` from PowerShell, run in the
  desktop session), on ClayWind's component demo:
  - the first screen's 77 elements with their control type, name, bounds and patterns; a disabled button says so;
  - `Invoke` on "Clicked 0" makes it "Clicked 1"; `Select` on a tab opens its page;
  - a slider: 0.5, a step up 0.6, a step down 0.5; `SetFocus` on a field gives it the focus, and typed keys are its
    value; a checkbox toggles Off to On; a radio button is selected and the other is not; a dropdown expands, an
    item of it is invoked, and the dropdown says the new value, collapsed.
  - `window_check` on the same laptop: 63 checks, 0 problems.
- **NVDA, the same day** (the owner: "Go ahead install it"; NVDA 2026.3, its speech read from its log while a
  script moved the pointer over the demo, pressed Tab and typed). What it says: "ClayWind — CeeSafe, window";
  under the pointer "Clicked 0", "Reset", "Dashboard", "Volume 50", "Quantity 3", "Save", "Subscribe to updates",
  "Dark", "Priority Medium", "README.md"; "Controls, tab, selected" when its page is opened; with Tab, "Name,
  edit", then the typed letters, "Note, edit", "Quantity, slider, 3". Four things it found, each fixed:
  - **No event reached any client**: the providers said they were client-side ones (`ProviderOptions` 1, where a
    server-side one is 2). Questions were answered all the same, which is why the first check passed.
  - **The focus was never told**, so NVDA went on believing the desktop was in front ("Foreground took too long to
    change") and said nothing of the window: UI Automation does not watch a window that has a provider of its own.
    The window's getting the focus and an element's taking it are now events.
  - **A deadlock**, once events were delivered: they were raised under the lock, and a client's question on
    another thread waited for it. They are raised with the lock let go.
  - **"Volume 0"** for a slider at 0.5 of 0 to 1: a reader says a slider's value as a whole number, so a slider
    that gives no value text is given as a percentage.
  And one that is the check's own: the name of plain text that changes each frame (a clock) raised an event each
  time; only a control's name does now.
- **Not checked**: Narrator; `announce`.

## 10. Layers (2026-11-18)

**Found** in the IPos port on ClayWind: an open dropdown's list lies over the invoice list's first row. A click there
picks the item (ClayWind's own hit test stops at a floating layer), but asked through the accessibility API (System
Events' `click at`, which presses the element `accessibilityHitTest:` names; VoiceOver's pointer the same) the point
was the row's, and pressing it opened an invoice. Every backend took "the last in the list that holds the point" as
the one on top. The list is in reading order, though (§2, §6: VoiceOver moves in it), and a dropdown is read where it
is, before the page it covers: drawn later, listed earlier.

**Decision.** `WindowAccessNode` has `layer: u32`: 0 for the page, higher for what is drawn over it. The element at a
point is, of those that hold it, the highest layer's, and of one layer the latest in the list, as before. The list's
order stays the reading order. A program that gives 0 everywhere has what it had.

- One function in the runtime (`cee_access_at`) answers it for macOS (`accessibilityHitTest:`) and Windows
  (`ElementProviderFromPoint`); Android's `AccessView` packs the layer with each node and its exploring finger does
  the same.
- **iOS** is unchanged: UIKit finds the element under a finger from `accessibilityElements` itself, and has no
  public way to ask the view.
- A node is eleven words to the runtime (the layer the eleventh).
- **Alternatives**: the list in drawing order (it would no longer be the reading order: a dropdown's items read after
  the page); a program giving no node under a popup (it would lose the page to the reader while a tooltip shows).
- **Checked** (macOS, `window_check --display`): Save is of layer 1 and a later node of layer 0 covers it;
  `accessibilityHitTest:` at Save's middle answers Save.

