# ADR 0268 — Sharing a file, and opening a file or a link with the system

- Status: **Accepted** by the owner 2026-11-27, with §6 answered as recommended (files and links; on Android a
  content provider of the app's, not a copy in Downloads; opening on every desktop, sharing on macOS), for Marivil's
  CeeUI port, whose row "the share sheet, opening a file in the system's viewer" was the media row's last "not yet".
- Scope: `lib/std/window.cee` (`ShareSheet`, `Launcher`), `runtime/hosted/ceesafe_rt_window.c` (dialog kinds 8, 9 and
  10 on iOS, Android, macOS, Windows, Linux and FreeBSD), `runtime/android/org/ceesafe/window/Sharing.java` (new) and
  `Picker.java`, type-system.md §26.13, `spec/api/std.md`, `tools/window_check.py`. The Android provider's class is
  the packer's (CeeUI's `ceeui-pack`), as ADR 0263's services are.

## 1. Context

- A messenger hands a photo or a document to another app (the share sheet: another chat app, mail, Files, Drive),
  opens a received document in the system's viewer (a PDF, a spreadsheet), and opens a link tapped in a message in
  the browser. MAUI has `Share.RequestAsync(ShareFileRequest)`, `Launcher.OpenAsync(OpenFileRequest)` and
  `Browser.OpenAsync`; CeeSafe has none of them, and nothing in its runtime starts another app.
- The systems:
  - iOS: `UIActivityViewController` (the share sheet, presented as the pickers are); `UIDocumentInteractionController`
    (a preview of the file, or "open in" another app); `UIApplication openURL:options:completionHandler:`.
  - Android: an `ACTION_SEND` (`ACTION_SEND_MULTIPLE`) intent through the chooser, an `ACTION_VIEW` intent for a file
    or a link. **Another app cannot read a file in the app's own folders**: it is handed a `content:` URI of a
    content provider the app declares in its manifest, with a temporary read grant for that one URI.
  - macOS: `NSSharingServicePicker` beside the window's view; `NSWorkspace openURL:`.
  - Windows: `ShellExecuteW` opens a file or a link; the share UI (`DataTransferManager`) needs WinRT interop.
  - Linux and FreeBSD: `xdg-open` (freedesktop's) opens a file or a link; there is no common share UI.

## 2. Decision

```
ShareSheet.share(mut window, ["photo.jpg"][0..1], "")?;     // the share sheet, for files and a text
Launcher.openFile(mut window, "report.pdf")?;               // the system's viewer
Launcher.openLink(mut window, "https://example.com")?;      // the browser (http, https, mailto, tel)
```

- **`ShareSheet.share(window, paths, text)`** offers the files at `paths` and `text` (either may be empty, not both)
  to the other apps, and returns when the sheet is closed on iOS, at once where the system does not say (Android's
  chooser, macOS's picker). Whether the user shared is not answered: Android does not tell, and a program has
  nothing to do either way.
- **`Launcher.openFile(window, path)`** opens the file in the app the system has for its type (iOS: a preview, or the
  apps that open it when it cannot preview it); **`Launcher.openLink(window, url)`** opens a link whose scheme is
  `http`, `https`, `mailto` or `tel` (any other is `Unsupported`, so that a link from a message cannot open a file of
  the app's or start an app by its own scheme).
- Errors (`WindowError`): `Unsupported` where the system has no such thing (sharing on Windows, Linux and FreeBSD;
  everything in a window without a backend and in the interpreter), when no app opens the file or link, and on
  Android when the app declares no provider; `Resources` when a file is not there or the system refused.
- **Android's provider** (§6.2): the runtime makes each file a `content://<package>.ceesafe.files/<path>` URI with a
  read grant for that intent. The provider, `org.ceesafe.Files` (exported false, `grantUriPermissions` true), is
  declared and compiled into the APK by the packer: it answers only for files inside the app's own folders (files,
  cache, and their external twins), read-only, with the name and size a receiver asks for. Nothing is copied.
- They run on the window's thread, as the file dialogs; a scripted window takes the next `dialog` answer, if any, and
  answers `Ok`.

## 3. Consequences

- Marivil's port shares a message's photo or file, opens a received document, and opens a message's links, on the
  phones and the Mac; Windows and Linux open but do not share.
- An Android APK made by a packer other than CeeUI's must declare the provider to share or open a file.

## 4. Options not taken

- **A copy in Downloads (or MediaStore) for each file shared**: no provider, but every file shared stays readable by
  every app, and Downloads fills up (§6.2).
- **Any link scheme**: a message could carry `file:`, `intent:` or an app's own scheme; the four cover what a
  messenger's links are, and more can be allowed later.
- **Windows' share UI** through WinRT's `DataTransferManager` (`IDataTransferManagerInterop` on a window handle): it
  can come later, as the Windows print dialog came after ADR 0178.

## 5. Tests

- `tools/window_check.py`: a scripted window answers the three; the library refuses a link of another scheme, a path
  with a newline, and a share of nothing, without the runtime.
- The Moto and the owner's iPhone in Marivil's port: a photo shared to another app, a PDF opened, a link opened.
- macOS: a file and a link opened, the sharing picker shown; Windows (the laptop): a file and a link opened.

Done (2026-11-27):
- `tools/window_check.py`: 70 checks, 0 problems (sharing and opening: the three answered, and eight refusals the
  library makes before the runtime). The window runtime compiles with `-Wall -Wextra -Werror` for macOS, iOS
  (arm64), Android (aarch64), Linux (gcc 14 and clang 19, Debian 13) and Windows (MinGW-w64 gcc 16, x64).
- The owner's Moto (Android 15), in Marivil's port with CeeUI's packer declaring `org.ceesafe.Files`: a chosen photo's
  Open started Google Photos with `content://com.murshisoft.marivil.ceeui.ceesafe.files/…` as `image/jpeg`, which
  showed it; Share opened the system's sheet, "Sharing image", its preview read through the provider (no app was
  chosen, so nothing was sent).
- macOS (arm64): `Launcher.openFile` opened a text file in TextEdit; a file not there was `Resources`.
- After the release (0.95.24, 2026-11-27):
  - The owner's iPhone 14 Plus, by his hands, in Marivil's port: a photo's preview (UIDocumentInteractionController),
    the share sheet, and a message's link in Safari.
  - The Moto: a message's link (sent by a test account) opened in Chrome (`ACTION_VIEW`).
  - macOS: the sharing picker beside a test window, offering the file ("note, Text Document · 42 bytes") to AirDrop,
    Notes and Messages (none chosen); a link opened in Chrome.
  - Windows 11 x64 (the owner's laptop, the released compiler, run in the owner's desktop session): a text file opened
    in Notepad, a link in Edge, a missing file `Resources`, a share `Unsupported`.
  - Fedora 45 (GNOME on Wayland, the released compiler): a text file opened in Text Editor and a link in Firefox
    through `xdg-open`, a missing file `Resources`, a share `Unsupported`.
- Not yet: FreeBSD on a desktop (its workflow builds and checks the runtime).

## 6. Questions for the owner (answered 2026-11-27)

1. **Scope**: files and links (recommended), or files only? *Files and links.*
2. **Android**: the runtime's own content provider (recommended: nothing copied, a grant per file), or a copy in
   Downloads? *The provider.*
3. **Desktops**: opening everywhere and sharing on macOS (recommended), or the phones only? *Opening everywhere, sharing
   on macOS.*
