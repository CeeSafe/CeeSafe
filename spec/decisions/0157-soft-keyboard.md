# ADR 0157 — The soft keyboard and the input method on iOS and Android

- Status: **Proposed**, 2026-11-16, for ClayWind's roadmap phase P4 (mobile) step 5, "text: the soft keyboard and
  the input method" (the owner's standing "continue"). The owner chose Android's route on 2026-11-16: "Small Java
  class in runtime" (§2.4), over key events alone and over iOS first. Not implemented.
- Scope: `lib/std/window.cee` (`Window.keyboardInset`, `WindowTextKind`, `Window.setTextInputKind`),
  `runtime/hosted/ceesafe_rt_window.c` (iOS, Android, the scripted window), a new `runtime/android/` (the Java
  source of one class and the generator of its embedded dex), type-system.md §26.13, `spec/api/std.md` (generated),
  `tools/window_check.py`. No compiler change.

## 1. Context

- **ADR 0141 gave a window text input** on the desktop systems: `setTextInput(on)`, `setTextInputArea(caret)`,
  committed text as `Text` events, and the text being composed as `Composition` with `composition()` and
  `compositionCaret()`. The program draws the composition; the window keeps it. On iOS and Android these calls do
  nothing yet (ADR 0154), so a text field there cannot be typed into: a phone has no other keyboard.
- **On a phone, text input is also what shows the keyboard.** The keyboard appears when a text editor takes focus
  and goes when it loses it, and it covers the lower part of the window while it is up.
- **Both systems' keyboards are input methods.** Every key goes through one, with composition on any language
  (suggestions, autocorrection, swipe typing, CJK), and each expects an editor object:
  - iOS: a responder that conforms to `UIKeyInput` (insert, delete) and, for marked text, `UITextInput`. The runtime
    already makes its classes through the Objective-C runtime.
  - Android: a `View` whose `onCreateInputConnection` returns an `InputConnection`. Both are Java; the NDK has no
    native form of them, and a NativeActivity's own view gives no editor, so an input method can send it only key
    events, which carry no composition and none of most keyboards' text.
- **ADR 0141's model fits a phone.** The program need not show its text to the input method: the editor the input
  method talks to holds only the composition, as on the desktop. What that costs is in §4.

## 2. Decision

### 2.1 The same calls, now also the keyboard

- **`setTextInput(true)`** on iOS and Android makes the window's editor the one the system types into, which
  shows the soft keyboard; **`setTextInput(false)`** ends a composition (ADR 0141) and hides it. A hardware keyboard
  attached to the phone types through the same editor, with no soft keyboard shown.
- **Committed text is `Text`**, one event per scalar; **the text being composed is `Composition`**, with
  `composition()` and `compositionCaret()`, exactly as ADR 0141 has them.
- **Editing keys the keyboard sends are `Key` events**: Backspace (iOS `deleteBackward`; Android
  `deleteSurroundingText` and the Delete key event) and Enter (a newline inserted on iOS; Android's editor action or
  Enter key event), down then up. While text is being composed they edit the composition instead and give no `Key`,
  as on the desktop.
- **`setTextInputArea`** is kept for the systems' own use (the caret's place for a floating keyboard's and a
  candidate window's position, where the system asks for it).

### 2.2 The keyboard's inset

```
let covered = w.keyboardInset();    // f32: points of the content's bottom that the soft keyboard covers; 0 without one
```

- **`w.keyboardInset() -> f32`** (`borrow`, no effect): how much of the content the soft keyboard covers, from the
  bottom edge, in points; 0 when none shows, with a hardware keyboard, on the desktop systems, and for a scripted
  window until its script says otherwise. It includes the part of the safe area's bottom inset the keyboard covers,
  so a program keeps clear of `max(safeArea().bottom, keyboardInset())`.
- **A change gives `Resized`** at the content's size, as a new safe area does (ADR 0156): a program that lays out
  at `Resized` moves its focused field above the keyboard with no new event, and `WindowEvent` is unchanged.
- **Not part of `safeArea()`**: the keyboard comes and goes with focus, and a program often wants one without the
  other (a full-height list behind the keyboard, a toolbar above it).
- The content does not shrink and the system does not pan it: iOS never does; on Android the runtime sets the
  activity's window to `SOFT_INPUT_ADJUST_NOTHING` when text input is first turned on.

### 2.3 The keyboard's kind

- **`WindowTextKind`**: `Text` (the default), `Number`, `Decimal`, `Email`, `Url`, `Password`.
- **`w.setTextInputKind(kind)`** (`borrow mut`): which keyboard the next `setTextInput(true)` shows, and for
  `Password`, that the input method neither learns nor suggests. A change while text input is on takes effect at
  once (the systems reload the keyboard). The desktop systems ignore it.

### 2.4 The systems

- **iOS.** `CeeSafeTouchView` becomes a text editor: `canBecomeFirstResponder`, and the `UITextInput` protocol's
  required methods over a document that is only the marked text (`insertText:`, `deleteBackward`, `hasText`,
  `setMarkedText:selectedRange:`, `unmarkText`, `markedTextRange`, `selectedTextRange`, positions and ranges as two
  small classes made the same way, `caretRectForPosition:` and `firstRectForRange:` from the text input area,
  `inputDelegate`, a `UITextInputStringTokenizer`). `setTextInput(true)` is `becomeFirstResponder`, false
  `resignFirstResponder`. The kind is the `UITextInputTraits` properties (`keyboardType`, `secureTextEntry`,
  `autocorrectionType`). The inset comes from `UIKeyboardWillChangeFrameNotification`, the keyboard's end frame
  against the view's bounds.
- **Android.** One Java class, `org.ceesafe.window.InputView` (a `View` whose `onCreateInputConnection` returns a
  `BaseInputConnection` subclass over an `Editable` that holds only the composition), is compiled when the runtime
  is built and embedded in it as a dex file of a few kilobytes, in a C array (`runtime/android/`: the Java source,
  and `tools/gen_android_dex.py`, which needs `javac` and the SDK's `d8` only when that source changes; the
  generated array is committed, so building a program needs neither).
  - **Loading**: at the first `setTextInput(true)`, on the activity's thread, the runtime loads the class from
    memory with `dalvik.system.InMemoryDexClassLoader` (API 26; ADR 0154's floor is 28), registers its native
    methods (`RegisterNatives`), makes one view, adds it to the activity's window (`addContentView`, 1×1,
    transparent) and gives it focus. An app's APK still has `android:hasCode="false"` and no Java of its own.
  - **Typing**: the connection's `commitText`, `setComposingText`, `finishComposingText`, `deleteSurroundingText`,
    `sendKeyEvent` and `performEditorAction` call the natives, which queue `Text`, `Composition` and `Key` for the
    program under the runtime's lock and wake its looper.
  - **Showing and hiding**: `InputMethodManager.showSoftInput` and `hideSoftInputFromWindow`, and `restartInput`
    for a new kind (`EditorInfo.inputType`, `imeOptions`).
  - **The inset**: `WindowInsets.getInsets(ime())` (API 30) read where ADR 0156 reads the others, with a listener
    on the view (`setOnApplyWindowInsetsListener`, in the Java class) so that a change is told at once; before API
    30, the visible display frame's bottom against the window's.
  - **Calls from the program's thread** (`setTextInput`, the kind, the area) are handed to the activity's thread
    through a pipe on its looper (`ALooper_forThread` in `ANativeActivity_onCreate`), since a view may be used only
    there. This also gives ADR 0154's missing clipboard a place to run, which is not part of this ADR.
- **Scripted windows**: `keyboard:H`, H a number of points, none negative: the keyboard's inset, with a `Resized`
  when it is new. `type:TEXT` and `compose:C:TEXT` are ADR 0141's.
- **Desktop systems**: `keyboardInset()` is 0 and `setTextInputKind` does nothing.

## 3. Alternatives considered

- **Android by key events alone** (show the keyboard through JNI, take `AKeyEvent`s). No Java, but an input method
  sends key events only for what a hardware key could type: no composition, no suggestions or swipe typing, and
  committed text that is no key (most CJK, emoji) arrives as a `KeyEvent` with `getCharacters`, which the NDK
  cannot read. Refused by the owner's choice.
- **Google's GameTextInput / GameActivity.** The same idea as §2.4 from a library, but an AndroidX dependency in
  every app's build and its own activity class in place of NativeActivity (ADR 0154).
- **Java in each app's APK** (a class the packaging adds). Every app would then need a Java toolchain to build, and
  the runtime would depend on a class it cannot count on being there.
- **The program's whole text given to the input method** (iOS's document, Android's `Editable` and
  `getTextBeforeCursor`), which lets it correct words already typed and capitalize after a full stop. It needs a new
  API in which the program and the system both edit the text, on every system; ADR 0141 chose the composition-only
  model, and this ADR keeps one model everywhere. It can be added later without breaking this one.
- **The keyboard as part of the safe area.** §2.2 says why not.
- **A `WindowEvent.Keyboard` variant.** It breaks every exhaustive `match`, as ADRs 0150, 0155 and 0156 found.

## 4. Consequences

- **No break**: no event or handle is added; a program that never turns text input on sees no change.
- **The runtime contains Java bytecode on Android**, a few kilobytes that only the Android build includes. This
  ends "no Java anywhere" for the runtime, not for apps, and adds one generator that needs a JDK and the Android
  SDK when the class changes. The contract suite checks that the committed array is what the generator writes,
  where those tools are present.
- **What the composition-only model costs on a phone**: the input method does not see the text around the caret,
  so it capitalizes the first letter after `setTextInput(true)` but not after a full stop, cannot correct a word
  once it is committed, and gives no suggestions from context. Typing, composition, swipe typing, CJK, emoji and
  dictation work.
- **ClayWind's text fields work unchanged**: its hosts already call `setTextInput` and `setTextInputArea` and draw
  `composition()`. They add the keyboard's inset to the bottom of the safe area for layout, and a field's kind.
- **Not covered**, each for a later ADR if a program needs it: the program's text shown to the input method
  (above); the system's selection handles, magnifier and edit menu (cut, copy, paste, look up); autofill and
  passwords from the system; the return key's label; an accessory bar above the keyboard; Android's clipboard.

## 5. Plan of implementation and evidence

1. The API in `lib/std/window.cee`, the shared layer (`cee_window_keyboard`, as `cee_window_safe_area`), the scripted
   `keyboard:H`, `tools/window_check.py` cases, type-system.md §26.13.
2. iOS: the editor on the view, the notification, the traits. Evidence in the iOS 27 simulator: the keyboard shown
   and hidden (screenshots), the inset at `Resized`, and text typed through the simulator's hardware keyboard path
   where that can be driven; the rest by the owner's hands.
3. Android: the Java class and its generator, the loader, the natives, the looper pipe, the inset. Evidence in the
   emulator: `adb shell input text` and key events through the input method, a composition from the emulator's
   keyboard, the inset upright and turned, API 28 to 37 where images are at hand.
4. The window runtime under ceesafec's own warning flags for every target; the contract suite; then ClayWind's
   hosts and its chat client's text field on both.
