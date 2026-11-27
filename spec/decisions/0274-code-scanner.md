# ADR 0274 — A code scanner: the camera's live picture over the app

- Status: **Accepted** by the owner 2026-11-27, with §6 answered as recommended (a live camera scanner, a QR decoder
  of CeeSafe's own where the system has none; the phones only), for Marivil's CeeUI port (scanning a contact's
  safety-number QR). Implemented the same day (§7).
- Scope: `lib/std/window.cee` (`CodeScanner`, `ScanState`), `runtime/hosted/ceesafe_rt_window.c` (dialog kind 15, the
  camera's permission; `ceesafe_scanner_*`), `runtime/android/org/ceesafe/window/Scanner.java` (new), `Picker.java`,
  type-system.md §26.13, `spec/api/std.md`, `tools/window_check.py`. The decoder is CeeSafeLibs' `qrcode`
  (`QrReader`, 2026-11-27). The declarations are the packer's (iOS's `NSCameraUsageDescription`, Android's `CAMERA`,
  CeeUI's `--camera`).

## 1. Context

- Marivil shows a contact's safety number as twelve groups of digits and a QR (`MRVL1:` and the digits); the other
  phone scans it, as MAUI's ScanQrPage does with ZXing.Net.MAUI's camera view.
- iOS reads QR codes itself: AVFoundation's `AVCaptureMetadataOutput`, with a preview layer of the session.
- Android's framework has no QR decoder (ML Kit and Google's code scanner are Play services' libraries): the camera
  (Camera2) gives frames, and the program reads them.

## 2. Decision

```
var scanner = CodeScanner.open(mut window)?;   // the camera's permission, then the camera over the app
match (scanner.poll()?) {                      // each frame of the app
    ScanState.Code => found(scanner.code()),   // iOS: the system read it
    ScanState.Frame => read(scanner.luminance(), scanner.width(), scanner.height()),  // Android: the program reads it
    ScanState.Closed => gone(),                // Cancel, or Back
    ScanState.Waiting => {}
}
```

- **`CodeScanner.open(window)`** asks the camera's permission once (`Resources` when refused) and shows the back
  camera's picture full screen over the app with a Cancel button; it stays until the program drops the scanner or the
  user closes it. One at a time.
- **`poll()`** never waits: a code the system read (`code()`, its text's bytes), a frame's luminance for the program
  (`luminance()`, `width()` by `height()`, about 640 by 480), the scanner closed, or nothing new.
- iOS gives codes, Android frames; CeeSafeLibs' `qrcode` reads a frame (`QrReader.read`) in about a millisecond.
- Windows, Linux, FreeBSD and macOS: `Unsupported`. A scripted window takes the permission from its next `dialog`
  item (none: refused) and has no scanner (`Unsupported`).

## 3. Consequences

- Marivil's safety screen gets Scan on the phones: the code read is compared with the contact's own.
- The program does the reading on Android, so any code `qrcode` reads works there, on any phone (no Play services).

## 4. Options not taken

- **Google's code scanner** (Play services' ready-made screen): the least Android code, but the APK bundles Play
  services' libraries and it fails on phones without them.
- **A photo, then a decoder**: no live picture; slower to use.
- **Frames on iOS too**: possible (a video data output), but AVFoundation's own reader is better on its own camera.

## 5. Tests

- `tools/window_check.py`: the scanner from a scripted window (no scanner), every method checked.
- CeeSafeLibs' `qrcode`: its codes read back from modules and from pictures (turned, in perspective, shaded, noisy),
  and a camera-like 640 by 480 frame of Marivil's safety screen.
- The Moto and the owner's iPhone, in Marivil's port.

## 6. Questions for the owner (answered 2026-11-27)

1. **How scanning works**: a live camera with CeeSafe's own decoder where the system has none (recommended), Google's
   code scanner, or a photo then a decoder? *The live camera and CeeSafe's decoder.*
2. **Systems**: the phones only (recommended), or the Mac too? *The phones only.*

## 7. Implementation (2026-11-27)

- `lib/std/window.cee`: `CodeScanner` (`@noncopy`, closed at its drop) and `ScanState` over `ceesafe_scanner_open`,
  `_poll` and `_close`; the permission is `FileDialog.run`'s kind 15. A frame larger than the program's buffer grows
  it for the next one.
- iOS (`ceesafe_rt_window.c`): an `AVCaptureSession` of the default video device with an `AVCaptureMetadataOutput` for
  `AVMetadataObjectTypeQRCode`, its delegate a `CeeSafeScanDelegate` made with the Objective-C runtime (on the main
  queue; it keeps the first code's `stringValue` and the Cancel), a presented `UIViewController` with an
  `AVCaptureVideoPreviewLayer` filling it and a Cancel `UIButton`; the permission `AVCaptureDevice`'s, after the app's
  `NSCameraUsageDescription` is looked for.
- Android: `Scanner.java` (in the dex): a full-screen dialog of a `TextureView` (the preview, cropped to fill it) and a
  Cancel button; the back camera through Camera2 with a repeating request to the preview and an `ImageReader`
  (YUV_420_888 nearest 640 by 480), whose Y plane is kept without its row padding while the program has taken the
  one before; `Picker` asks `CAMERA` (kind 15). An app declaring `CAMERA` has `Picker`'s capture ask it too
  (ADR 0265's capture intent needs it then), which it did already.
- The window runtime compiles with `-Wall -Wextra -Wcast-qual -Werror` for macOS (arm64, x86_64), iOS, Android, Linux
  (gcc 14, clang 19) and Windows (MinGW-w64 gcc 16).
