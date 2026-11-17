# ADR 0161 — What a soft keyboard's Return key says

- Status: **Accepted** by the owner, 2026-11-17 ("From me it's Go ahead for all the decisions mentioned above.", to
  "a Done label on the keyboard's Return key needs a small CeeSafe API addition (an ADR and a release). Want it?"),
  for ClayWind's roadmap phase P4 (mobile) step 9, the chat client on phones: the owner found no key on an iPhone's
  keyboard that said it would put the keyboard away. Implemented on branch `adr/0161-return-key` (from release
  0.62.1, rebased onto 0.62.2): iOS and Android (§5).
- Scope: `lib/std/window.cee` (`WindowReturnKey`, `Window.setReturnKey`), `runtime/hosted/ceesafe_rt_window.c` (iOS,
  Android), `runtime/android/org/ceesafe/window/InputView.java` and its embedded dex, type-system.md §26.13,
  `spec/api/std.md` (generated), `tools/window_check.py`. No compiler change.

## 1. Context

- **A soft keyboard has one key whose face the app chooses** (ADR 0157 gave the keyboard and its kind of text): iOS's
  Return key says "return", "done", "go", "next", "search" or "send"; Android's action key shows the same as a word
  or a sign.
- **The key already reaches the program**, as `Key.Enter` (ADR 0157): iOS through `textFieldShouldReturn:`, Android
  through `performEditorAction` and the Enter key. What Enter does in a field is the program's: ClayWind's text
  field leaves the field, which puts the keyboard away.
- **So the missing part is the face.** With "return" on the key, nothing on the keyboard says that pressing it ends
  the typing; the owner asked for a button named "done".

## 2. Decision

```
window.setReturnKey(WindowReturnKey.Done);   // before or while the keyboard is shown
```

- **`WindowReturnKey`**: `Default` (the system's own), `Done`, `Go`, `Next`, `Search`, `Send`.
- **`w.setReturnKey(key)`** (`borrow mut`, no effect): what the Return key says in the focused field, on iOS and
  Android, at once when a keyboard is shown; desktop systems and a scripted window ignore it. Until the first call
  it is `Default`.
- **The face only.** The key gives `Key.Enter` whichever it shows; no new event, and no `WindowEvent` variant.
- **Backends.**
  - iOS: the hidden field's `returnKeyType` (`UIReturnKeyDefault` 0, `Done` 9, `Go` 1, `Next` 4, `Search` 6, `Send`
    7), set with the field's other traits, and `reloadInputViews` when it is first responder, as a new kind of text
    does.
  - Android: `EditorInfo.imeOptions`' action (`IME_ACTION_NONE`, `DONE`, `GO`, `NEXT`, `SEARCH`, `SEND`), with the
    input restarted when it changes, as for a new kind of text. `InputView.apply` takes it in its `kind` argument's
    second byte, so the JNI signature is ADR 0157's.

## 3. Alternatives considered

- **A `Done` key that hides the keyboard itself**, with no event. A program would not know the field was left; and
  a "send" key must reach the program anyway.
- **A toolbar over the keyboard** with a Done button (iOS's `inputAccessoryView`). More to draw and to lay out for,
  on one system only; the Return key is there on both.
- **Folding it into `WindowTextKind`** (`EmailDone`, …). Two independent choices as one enum's product.
- **Free text for the key.** Neither system takes any: both choose among fixed faces.

## 4. Consequences

- **No break**: one enum and one method more; a program that never calls it has the key it had.
- **ClayWind** reports a field's Return key as it reports its kind of text; its text fields say `Done` unless told.
- **Not covered**: iOS's `enablesReturnKeyAutomatically` (the key greyed while the field is empty); iOS's other
  faces (`Join`, `Route`, `Continue`, `Emergency call`); a toolbar over the keyboard.

## 5. Implementation and evidence (2026-11-17)

- `lib/std/window.cee`: `WindowReturnKey`, `Window.setReturnKey`; `ceesafe_window_set_return_key` keeps the number
  in the window and, on iOS and Android, applies it as a new kind of text is applied (`cee_native_text_kind`).
- iOS: `cee_ios_field_traits` sets `returnKeyType`. Android: `cee_android_kind` packs the two numbers for
  `InputView.apply`; the dex is regenerated (`tools/gen_android_dex.py --check`: 0 problems).
- `tools/window_check.py`: 58 checks, 0 problems; its keyboard program calls `setReturnKey`, which a scripted window
  accepts and ignores.
- A moto g35 5G (Android 15), ClayWind's chat client built with the branch's compiler, by screenshots: the action key
  is a tick in a field that says `Done`, an arrow in one that says `Go`, a paper plane in one that says `Send`, each
  at once when the focus moves to the field; Enter from the `Go` key connected, as Enter did before.
- Not verified: the faces on an iPhone at the time of release (the build is on the owner's iPhone 14 Plus); a
  scripted window and the desktop systems only in that they accept the call.
