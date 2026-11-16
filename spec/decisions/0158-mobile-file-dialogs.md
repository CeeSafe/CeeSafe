# ADR 0158 — File dialogs on iOS and Android

- Status: **Accepted** by the owner, 2026-11-16 ("Yes, I accept accept ADR 0158 as written."). The owner, with
  ClayWind's demo on his iPhone 14 Plus and moto g35 5G: "I noticed that file open and file save are not working on
  both ios and android". Implemented on branch `adr/0158-mobile-file-dialogs` (§5).
- Scope: `lib/std/window.cee` (`FileDialog.openFile` and `openFiles` on iOS and Android; a new
  `FileDialog.exportFile` on every system), `runtime/hosted/ceesafe_rt_window.c`, `runtime/android` (the Java
  class), type-system.md §26.13, `spec/api/std.md` (generated), `tools/window_check.py`. No compiler change.

## 1. Context

- **`FileDialog` (ADR 0146) is the desktop's**: `openFile`, `openFiles`, `saveFile` and `chooseFolder` are modal and
  answer with absolute paths, which the program then reads or writes with `File`. On iOS and Android they are
  `Unsupported` (ADR 0154), so an app's Open and Save do nothing there.
- **A phone gives an app no paths outside its own container.** What its picker gives is a grant to one document:
  - iOS: `UIDocumentPickerViewController`. Opening gives a security-scoped URL, or, asked for a copy, a file in the
    app's own temporary folder. Saving is an export: the app hands the picker a file it has already written, and the
    user chooses where it goes.
  - Android: the Storage Access Framework. `ACTION_OPEN_DOCUMENT` gives a `content://` URI, read through a
    `ContentResolver`, never a path; `ACTION_CREATE_DOCUMENT` gives a URI to write to. Both answer through
    `onActivityResult`, which is Java.
- **`saveFile` cannot mean on a phone what it means on a desktop**: it returns where to write before anything is
  written, and a phone's picker wants the bytes first (iOS) or gives no path to write to (Android).

## 2. Decision

### 2.1 Opening: the same calls, the path of a copy

- **`FileDialog.openFile` and `openFiles` work on iOS and Android** with their desktop signatures. They show the
  system's picker and return when the user answers: null, or empty, at a cancel.
- **The paths they return are of copies in the app's own temporary folder**, made by the system (iOS, asked for a
  copy) or by the runtime (Android, from the URI's stream), named as the documents are. The program reads them with
  `File` as on the desktop. Writing to one does not change the user's document; a program that edits a document
  saves it with `exportFile`.
- **`extensions`** limits what can be picked, as on the desktop: mapped to uniform type identifiers on iOS and to
  MIME types on Android; an extension the system does not know offers every file.
- The copies are the app's to delete; the system clears the temporary folder when it needs the room.

### 2.2 Saving: `exportFile`, on every system

```
File.write(draft, bytes)!;                                              // a file the app has written
let saved = FileDialog.exportFile(mut window, "Save As", draft, "notes.txt")!;   // Result<bool, WindowError>
```

- **`FileDialog.exportFile(window, title, file, suggestedName) -> Result<bool, WindowError>`**: gives the existing
  file at `file` to the user to save where they choose, under `suggestedName` unless they change it. True when it
  was saved, false at a cancel. The program's file is left as it is.
  - iOS: the document picker for exporting, with a copy.
  - Android: `ACTION_CREATE_DOCUMENT`, then the file's bytes written to the URI it gives.
  - Desktop systems: the save dialog of `saveFile`, then a copy to the chosen path, so one program saves the same
    way everywhere.
- **`saveFile` stays the desktop's** and stays `Unsupported` on iOS and Android, where it cannot keep its meaning.
  A desktop program that wants the chosen path (to show it in a title bar, to save again without asking) keeps
  using it.
- **`chooseFolder` stays `Unsupported`** on iOS and Android: a folder's grant is a tree of URIs, not a path.

### 2.3 Modal, as on the desktop

- The call returns when the user has answered. iOS: the picker is presented by the window's view controller and the
  run loop runs inside the call until its delegate answers. Android: the program's thread waits on its looper
  inside the call while the picker's activity is in front (the app's own window is gone meanwhile, and comes back).
- Events that arrive meanwhile (focus lost and gained, a `Resized`) are queued for after the call, as they are
  around a desktop dialog.

### 2.4 The systems

- **iOS**: `UIDocumentPickerViewController` through the Objective-C runtime: `initForOpeningContentTypes:asCopy:`
  with `UTType`s made from the extensions (`typeWithFilenameExtension:`; `public.item` for none), `allowsMultipleSelection`
  for `openFiles`, and `initForExportingURLs:asCopy:` for `exportFile`; a delegate class of the runtime's with
  `documentPicker:didPickDocumentsAtURLs:` and `documentPickerWasCancelled:`.
- **Android**: the runtime's dex (ADR 0157) gains a second small class, a headless `android.app.Fragment` added to
  the activity, since only an activity or a fragment receives `onActivityResult` and an app has no Java of its own.
  It starts the intent, and at the result copies each picked document to the cache folder (open) or writes the
  program's file to the created document (export) on a worker thread, then tells the runtime, which ends the wait.
  `android.app.Fragment` is deprecated since API 28 and present in every release so far; the AndroidX replacement
  would be a dependency in every app.
- **Scripted windows**: `dialog:PATH|PATH…` already answers the open dialogs (ADR 0146); it answers `exportFile`
  too, by copying the file to the script's path.

## 3. Alternatives considered

- **A handle instead of a path** (`PickedFile` with `read` and `write`, backed by a security-scoped URL or a content
  URI), which edits the user's document in place and needs no copy. It is the model the systems have, and it is
  what an editor of large documents wants; but it is a second file API beside `File`, on every system, for every
  program that opens a file. The copy keeps `File` the one way to read and write, and a handle can be added later
  for the programs that need it.
- **`saveFile` returning a staging path on a phone**, exported by a later call or when the window next polls. The
  runtime cannot know when the program has finished writing, and a save that happens some time after the call
  returns, or never, is a silent failure.
- **The app's own Documents folder shown in the Files app** (`UIFileSharingEnabled`, Android's app-specific
  storage). It needs no picker but is not what Open and Save mean to a user: they cannot pick a file from elsewhere.
- **A proxy activity in each app's manifest** to receive the result. An in-memory class cannot be an activity (the
  system loads activities by the app's class loader), so every app would ship Java.

## 4. Consequences

- **No break**: the existing calls keep their signatures and their desktop behaviour; `exportFile` is new.
- **A program that saves on every system uses `exportFile`**: it writes a file of its own, then exports it.
  ClayWind's demo and chat client change their Save to that.
- **An opened document is a copy** on a phone: large documents cost their size in the app's temporary folder, and
  the picked document's own place is not known to the program.
- **Not covered**, each for a later ADR if a program needs it: documents edited in place (the handle above), folders,
  the photo library's picker, sharing sheets, files opened from another app ("Open in…").

## 5. Implementation and evidence (2026-11-16)

- **The dialog's entry** (`ceesafe_window_dialog`) has a fifth kind, an export: `name` is the suggested name and
  `extensions` carries the file. On iOS and Android the backend saves the file and the answer is one empty path;
  elsewhere the answer is the save dialog's path, and `lib/std` copies the file to it.
- **iOS**: the picker for opening (`initForOpeningContentTypes:asCopy:`) and for exporting
  (`initForExportingURLs:asCopy:`, of a copy under the suggested name in the temporary folder), a delegate class,
  `UTType`s from `UniformTypeIdentifiers`, loaded at the first dialog.
  - **Found on the owner's iPhone**: after a save, the next dialog was answered "cancelled" at once while its picker
    stayed on the screen, and the dialog after it never appeared, so the program waited for ever. A picker may tell
    its delegate again, late, as it leaves; and UIKit does not present over a controller that is going away. Now a
    dialog takes only its own picker's first answer, dismisses the picker if it has not begun to go, and returns
    once it has left (five seconds at most).
- **Android**: `runtime/android/org/ceesafe/window/Picker.java`, in the same dex as `InputView` (about 12 kilobytes
  now; `tools/gen_android_dex.py` hashes both sources). The runtime's classes are looked for in the app's own class
  loader first, then in the embedded dex, so an app whose APK holds them has a fragment the system can make again
  after it ended the app's process behind a picker; without that, the activity that comes back then fails once. The
  program's thread waits on its looper inside the call; the activity's end answers a cancel.
- **`lib/std/window.cee`**: `FileDialog.exportFile`; `spec/type-system.md` §26.13.
- **Evidence:**
  - `tools/window_check.py`: 54 checks, 0 problems, on macOS arm64. Its new one: on a scripted window an export
    copies the file to the script's answer and is true, leaves the file as it was, is false at a cancel, and is
    `Resources` for a file that does not exist.
  - Android 17 emulator (API 37; built for API 28, targeting 36), a test that writes a draft, exports it, opens one
    text file and then several, with the picker driven by taps: the save screen came with the suggested name, and
    the saved file in Downloads held the draft's line; the opened file was a copy under the app's
    `cache/ceesafe-picked` with the document's name and text; two selected files came back as two copies; Back in
    each picker gave a cancel (`false`, null, none); `saveFile` was `Unsupported`; `main` returned 0.
  - The owner's moto g35 5G (Android 15), by his hands: the export saved; the file he then picked was that export,
    read back with its line; one file selected in the last picker came back as one copy; `main` returned 0.
  - The owner's iPhone 14 Plus, by his hands, after the fix above: the export saved; the picked file was a copy in
    the app's `tmp/…-Inbox` with the export's line; one file from the last picker likewise; `saveFile` was
    `Unsupported`; the program ended with code 0. The iOS simulator showed the export's picker.
  - ClayWind's demo, whose Save writes its notes to a file in `Directory.temporary()` and exports it: on the
    emulator, Save As gave "Saved notes.txt" and the file in Downloads, and Open of it "Opened notes.txt"; on both
    of the owner's phones, by his hands: "On both Ios and Android, file operations were perfect."
  - Met on the way, in `runtime/hosted/ceesafe_rt_io.c`, and fixed: `Directory.temporary()` failed on Android,
    which sets no `TMPDIR` for an app and has no `/tmp` (an app's temporary directory there is now its cache folder,
    `/data/data/<package>/cache`, the package read from `/proc/self/cmdline`; a program run from a shell gets
    `/data/local/tmp`); and a temporary directory's name began "ceesa", five bytes of "ceesafe-" being copied.
  - `tools/contract_tests.py` with `out/ceesafec` built from the branch: every check ok but `signal_check.py` (5
    problems of 27 cases), which ran while the machine was building for the phones and running an emulator; alone it
    passes, four runs of four.
  - Not verified: several files picked at once on the iPhone, a cloud provider's documents (iCloud Drive, Google
    Drive), a large document, the app's process ended behind a picker on Android, Android before API 30.
