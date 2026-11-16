# ADR 0157 — The soft keyboard and the input method on iOS and Android

- Status: **Accepted** by the owner, 2026-11-16 ("Yes, I accept ADR 0157. Go ahead"), for ClayWind's roadmap phase
  P4 (mobile) step 5, "text: the soft keyboard and the input method". The owner chose Android's route the same day:
  "Small Java class in runtime" (§2.4), over key events alone and over iOS first. Implemented on branch
  `adr/0157-soft-keyboard` (§5), with two changes from the proposal, both in §2.4: iOS's editor is a hidden
  `UITextField` rather than the runtime's own `UITextInput`, and Android's inset is 0 before Android 11.
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

- **iOS.** A hidden `UITextField` in the window's view, `CeeSafeTextField` (a subclass made through the
  Objective-C runtime), is the editor, so UIKit's own `UITextInput` talks to the input method. (The proposal had the
  runtime implement `UITextInput` on the view itself: some thirty methods and two position classes for a document
  that is only the marked text, where the text field has them all and is the editor every keyboard is tested with.)
  - `setTextInput(true)` empties the field and makes it first responder; false resigns it.
  - At each change of the field (`UIControlEventEditingChanged`): text outside the marked range is committed (`Text`)
    and, once nothing is marked, taken out of the field; the marked text is the composition, its caret at its end.
  - `deleteBackward` in an empty field is Backspace; `textFieldShouldReturn:` is Enter.
  - The field never holds more than the composition, so automatic capitals, correction and spell checking are off
    (`autocapitalizationType`, `autocorrectionType`, `spellCheckingType`): with them on, every letter would begin a
    sentence. The kind is `keyboardType` and `secureTextEntry`, with `reloadInputViews` for a change.
  - The inset comes from `UIKeyboardWillChangeFrameNotification`: the screen's height less the end frame's top.
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
  - **The inset**: `WindowInsets.getInsets(ime())` (API 30) read where ADR 0156 reads the others, and when the
    view's `onApplyWindowInsets` runs, so that a change is told at once. Before Android 11 it is 0: the window does
    not pan (`SOFT_INPUT_ADJUST_NOTHING`), and those systems tell a window nothing of a keyboard it does not resize
    for.
  - **A hardware keyboard**: its key events that neither the input method nor the program's input queue took reach
    the view's `onKeyDown`, and are typed as the soft keyboard's are.
  - **Back hides the keyboard** without ending text input, and the program is not told. A tap on the window while
    text input is on and no keyboard shows asks for it again.
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
  SDK when the class changes. The array records the SHA-256 of the source it was made from, and the contract suite
  checks that against the source (the bytes differ from one javac or d8 to the next, so they are not compared).
- **What the composition-only model costs on a phone**: the input method does not see the text around the caret,
  so it does not capitalize the start of a sentence (on iOS no letter, automatic capitals being off), cannot correct
  a word once it is committed, gives no suggestions from context, and a word typed by swiping gets no space before
  it. Typing, composition, CJK, emoji and dictation work.
- **ClayWind's text fields work unchanged**: its hosts already call `setTextInput` and `setTextInputArea` and draw
  `composition()`. They add the keyboard's inset to the bottom of the safe area for layout, and a field's kind.
- **Not covered**, each for a later ADR if a program needs it: the program's text shown to the input method
  (above); the system's selection handles, magnifier and edit menu (cut, copy, paste, look up); autofill and
  passwords from the system; the return key's label; an accessory bar above the keyboard; Android's clipboard.

## 5. Implementation and evidence (2026-11-16)

- **The shared layer** (`runtime/hosted/ceesafe_rt_window.c`): `cee_window_keyboard` keeps the inset, at most the
  content's height, and queues `Resized` at a change; `cee_window_commit` (`Text` for each scalar, then no
  composition) and `cee_window_key_tap` for the mobile backends; `cee_window_text` is now every backend's.
- **iOS**: `CeeSafeTextField` and its four methods, the field made at the first `setTextInput(true)`, the keyboard
  notification, the traits.
- **Android**: `runtime/android/org/ceesafe/window/InputView.java` (a view and its `BaseInputConnection`), compiled
  by `tools/gen_android_dex.py` (javac `--release 11`, d8 `--release --min-api 28`) into a dex of about 5 kilobytes
  between two marker lines of the window runtime, with the source's SHA-256; `--check` compares that with the source.
  The loader, the three natives (text as UTF-8 bytes, not a Java string, whose JNI form is not UTF-8), the queue of
  what was typed, the pipe on the activity's looper, the keyboard's inset beside the safe area's.
- **`lib/std/window.cee`**: `WindowTextKind`, `Window.keyboardInset`, `Window.setTextInputKind`;
  `spec/type-system.md` §26.13 and its script grammar (`keyboard:H`).
- **Evidence:**
  - `tools/window_check.py`: 53 checks, 0 problems, on macOS arm64; with `--display`, 64. Its three new ones: a
    trace (zero at first; a keyboard with a `Resized` at the same size and a typed letter; the same again gives
    nothing; one taller than the content is the content's height; zero again), and keyboards of two numbers and of
    a negative one as malformed scripts. With `--display`, a real macOS window answers 0.
  - iOS 27 simulator (iPhone 18 Pro), a test that turns text input on, prints every event, and has a C helper call
    the field's own methods as the input method does (the Simulator's keyboard is not driven here): the keyboard
    shows (screenshot) and `Resized` gives an inset of 301 points; `insertText:` of "Hi 😀" gives four `Text`;
    `setMarkedText:` of "かな" gives `Composition` with that text; `insertText:` of "仮名" over it gives two `Text`
    and an empty composition; `deleteBackward` in the empty field gives Backspace down and up; Return gives Enter;
    a marked "x" deleted gives the composition and then none; the number pad gives an inset of 302; text input off
    gives 0.
  - Android 17 emulator (API 37, arm64; built for API 28, targeting 36), the same test: the dex loads and Gboard
    starts for the view (`inputType=1`), inset 336.4 points; taps on Gboard's own keys (h, i, space, o, Backspace,
    o, i, Enter) give those `Text`s and keys in order; `adb shell input text hello` and key events 67 and 66 (the
    hardware keyboard's path) give five `Text`s, Backspace and Enter; the number kind restarts Gboard with
    `inputType=2`; turned to landscape the inset is 261.3 of 411 points, and back 336.4; Back hides the keyboard
    (0) and a tap shows it again (336.4), after which a key types; Back twice more gives `Closed` and `main`
    returns 0, with no crash in the log. While the screen turned, the system gave two insets in passing (one
    larger than the content, kept to its height), each with its `Resized`.
  - The window runtime compiles with ceesafec's own warning flags and `-Werror` for macOS (arm64, x86_64), the iOS
    simulator (arm64, x86_64), iOS devices (arm64), Android API 28 (arm64, x86_64, armv7; NDK 28.2), Linux (Ubuntu
    24.04, gcc 13) and Windows (MinGW-w64 gcc 13).
  - The owner's phones, the same test. A moto g35 5G (Android 15, API 35, 1080×2400 at density 400, over wireless
    debugging): Gboard shown, inset 370 points; `input text hello` and key events gave five `Text`s, Backspace and
    Enter; turned, inset 247.2, and the number pad 213.2; back upright 336 with the number pad, after three insets in
    passing; off gave 0 and `main` returned 0. ClayWind's demo there: a tap on its Name field showed the keyboard,
    and taps on Gboard's keys (h, i, space, o, Backspace, i) left "hi i" in the field. An iPhone 14 Plus (built from
    ceesafec's emitted C in an Xcode project signed for the device, installed and launched with `devicectl`):
    428×926 at 3, safe area 0 47 0 34, keyboard inset 320, the helper's steps as in the simulator, the number pad
    321; then the owner typed "heloo" and Return on the phone's keyboard, which gave those five `Text`s and Enter.
  - `tools/contract_tests.py` with `out/ceesafec` built from the branch: every check ok, after the seven check-pass
    HIR dumps were regenerated (closure numbers after lib/std's, as for ADR 0156). The suite's new step,
    `gen_android_dex.py --check`, was added after that run and passes alone.
  - Not verified: swipe typing and a composing keyboard (Japanese, Chinese, Korean) on either system (the emulator's
    and the phone's Gboard committed each letter as typed), Backspace and a turn of the screen by hand on the
    iPhone, Android before API 30, dictation, a hardware keyboard on iOS.
