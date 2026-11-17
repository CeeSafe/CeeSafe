# ADR 0164 — What a window holds, told to a screen reader

- Status: **Proposed**, 2026-11-17, for ClayWind's roadmap phase P5 (platform completeness; the owner: "Continue to
  P5"), whose gate is "the component library passes VoiceOver, NVDA and TalkBack checks". Nothing is implemented in
  CeeSafe. ClayWind's side exists (its commit `3b23725`): a frame's elements with role, name, value, state and place,
  and a screen reader's actions as the controls see them; it names no system, and waits for this.
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
  are; `min`, `max`, `now: f64` for a slider; and `labelStart`, `labelLength`, `valueStart`, `valueLength: u32`,
  ranges of the `text` given with the list (UTF-8).
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
