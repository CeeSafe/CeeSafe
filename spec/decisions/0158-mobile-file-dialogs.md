# ADR 0158 — File dialogs on iOS and Android

- Status: **Proposed**, 2026-11-16. The owner, with ClayWind's demo on his iPhone 14 Plus and moto g35 5G: "I
  noticed that file open and file save are not working on both ios and android". Not implemented.
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

## 5. Plan of implementation and evidence

1. `exportFile` in `lib/std/window.cee` and on the desktop systems and the scripted window; `tools/window_check.py`
   cases; type-system.md §26.13.
2. iOS: the picker, its delegate, the modal wait. Evidence: the simulator and the owner's iPhone, a text file opened
   from Files and exported back.
3. Android: the fragment, the copies, the modal wait. Evidence: the emulator and the owner's moto g35 5G.
4. The window runtime under ceesafec's own warning flags for every target; the contract suite; ClayWind's demo's
   Open and Save on both phones.
