# ADR 0265 — The photo library and the camera: picking photos and videos, and capturing them

- Status: **Accepted** by the owner 2026-11-26, for Marivil's CeeUI port (phase 3: "pickers and camera"), with §6
  answered: several at once, photos always JPEG, the storage permission on Android 7.1 to 9, and a capture **kept in
  the gallery** (not the recommendation; §2 says how).
- Scope: `lib/std/window.cee` (`MediaPicker`), `runtime/hosted/ceesafe_rt_window.c` (iOS and Android; the desktop's
  through `FileDialog`), `runtime/android/org/ceesafe/window/Picker.java`, type-system.md §26.13, `tools/window_check.py`;
  CeeUI's packer (`--camera`, `--microphone`). Follows ADR 0158 (file dialogs on phones), which left "the photo
  library's picker" for a later ADR (§4).

## 1. Context

- **What Marivil does** (its MAUI client, surveyed 2026-11-26): a chat attachment is a photo or a video from the
  library, a photo or video from the camera (a video note is a video), or any document (`FileDialog.openFile` already
  serves that); the avatar, a group's photo and a status are a photo from the library or the camera. Every pick is
  one item. Photos are then made JPEG (1600 pixels at most, quality 0.82, a 640-pixel thumbnail), videos re-encoded
  on iOS (720p), a poster frame taken; QR codes are scanned with the camera; calls use it too.
- **A phone's library is not files.** `FileDialog.openFile` on a phone (ADR 0158) shows the documents picker (Files,
  Drive), not the photo library; users expect the gallery's grid.
  - iOS 14 and later: `PHPickerViewController`, out of process, needing no permission. A photo is often HEIC and may be
    in iCloud only; its `preferredAssetRepresentationMode` "compatible" has the system transcode to JPEG and H.264.
    CeeUI's apps start at iOS 17.
  - Android: the photo picker, `MediaStore.ACTION_PICK_IMAGES`, needing no permission (Android 13, and Android 11
    and 12 through Google Play's system updates, `SdkExtensions` R ≥ 2); before it, `ACTION_GET_CONTENT` with image
    and video types. A photo may be HEIF on some phones.
- **The camera's own app** captures without the app holding a camera:
  - iOS: `UIImagePickerController` with the camera as its source; it needs `NSCameraUsageDescription` (and
    `NSMicrophoneUsageDescription` for a video, which records sound).
  - Android: `ACTION_IMAGE_CAPTURE` and `ACTION_VIDEO_CAPTURE`, which write to a `content://` URI the app gives
    (`EXTRA_OUTPUT`). A CeeSafe app has no Java of its own for a `FileProvider`; a new item in `MediaStore` is
    such a URI with no permission from Android 10 (not a pending one: only its owner may write that), and is the
    gallery's item. If the app's manifest declares
    `CAMERA` (for its own scanning, later), the intent is refused until that permission is granted.
- **Not here**: making JPEG and scaling (a library's: CeeUI decodes JPEG with its EXIF orientation, nothing encodes
  it yet), video re-encoding and poster frames, the camera's frames for scanning, the share sheet and opening a file
  in the system's viewer, saving a received photo to the gallery. Each is its own decision when the port reaches it.

## 2. Decision

### 2.1 The calls

```
public enum MediaKind { Photo, Video }
public struct PickedMedia { public path: String; public kind: MediaKind; }

MediaPicker.pick(mut window, photos: true, videos: false, limit: 1)   // Result<Vector<PickedMedia>, WindowError>
MediaPicker.capture(mut window, MediaKind.Photo)                       // Result<PickedMedia?, WindowError>
MediaPicker.canCapture(MediaKind.Video)                                // bool
```

- **`pick`** shows the system's photo library with photos, videos or both, and lets the user choose up to `limit`
  items (1: one); empty at a cancel. **`capture`** opens the camera for a photo or a video; null at a cancel. A
  capture is also **kept in the system's gallery** (§6.4), as the camera app's own photos are.
  **`canCapture`** says whether this device and system have a camera for it (false on a desktop, in a simulator).
- **Modal, and copies**, as ADR 0158's dialogs: the call returns when the user has answered, with paths of files in
  the app's temporary folder that the program reads with `File` and may delete; events that come meanwhile are queued
  for after it.
- **What a file is**: a photo is **JPEG** (HEIC and HEIF made JPEG by the system or the runtime, at quality 0.9, so a
  program reads every photo the same way); a GIF, PNG or WebP stays as it is. A video is as the system gives it:
  MP4 or QuickTime, H.264 on iOS (the compatible representation). The file's extension says which.

### 2.2 The systems

- **iOS**: `pick` is `PHPickerViewController` (a `PHPickerConfiguration` with the filter, `selectionLimit` and the
  compatible representation mode), each result's file copied with `loadFileRepresentationForTypeIdentifier:` (an
  item in iCloud only is downloaded then). `capture` is `UIImagePickerController` (camera, the photo or movie type):
  a photo from its `UIImage` as JPEG at 0.9, a video from its media URL; then the photo or video is added to the
  library (`UIImageWriteToSavedPhotosAlbum`, `UISaveVideoAtPathToSavedPhotosAlbum`; iOS asks once to add to Photos),
  when the app's Info.plist has `NSPhotoLibraryAddUsageDescription` (without it iOS would end the app; the capture is
  then the app's only). Both presented by the window's controller as
  the document picker is (ADR 0158 §2.4, its fix for a late second answer included).
- **Android**: `Picker.java` gains the kinds. `pick`: `ACTION_PICK_IMAGES` where it exists (with
  `EXTRA_PICK_IMAGES_MAX` above 1, the image or video type), else `ACTION_GET_CONTENT` with `EXTRA_MIME_TYPES` and
  `EXTRA_ALLOW_MULTIPLE`; each URI copied to `cache/ceesafe-picked` as the documents are, a HEIF made JPEG through
  `ImageDecoder` (API 28 and later). `capture`: a new `MediaStore` item (in Pictures or Movies, under the app's name)
  as `EXTRA_OUTPUT`, which the camera's app fills, so it is in the gallery, and which is copied to the cache for the
  program; at a cancel it is deleted. It is not a pending item: only its owner may write one (on the Moto the
  camera's app ended at its confirm, §5). The `CAMERA` permission is asked first when the
  app declares it; on Android 7.1 to 9 (API 25 to 28) `WRITE_EXTERNAL_STORAGE` is asked first (§2.3).
- **Desktops**: `pick` is `FileDialog.openFiles` (or `openFile` for a limit of 1) with the photo and video
  extensions, so a desktop program picks the same way; `capture` is `Unsupported` and `canCapture` false.
- **Scripted windows** (ADR 0146): `dialog:PATH|PATH…` answers `pick` and `capture` as it answers the file dialogs.

### 2.3 Building

- CeeUI's packer: `--camera TEXT` (iOS's `NSCameraUsageDescription`; on Android, `WRITE_EXTERNAL_STORAGE` with
  `maxSdkVersion="28"`), `--microphone TEXT` (`NSMicrophoneUsageDescription`, for video capture) and
  `--save-photos TEXT` (`NSPhotoLibraryAddUsageDescription`, for keeping captures in Photos); a picker needs nothing.
  An app that captures without the camera's text is ended by iOS at the first `capture`, so `capture` checks the
  Info.plist first and answers `Resources` ("the system refused") instead.

## 3. Consequences

- Marivil's attachments, avatar, group photo and status pick and capture through `MediaPicker`; its "Document" entry
  stays `FileDialog.openFile`.
- A program gets JPEG for every photo, which CeeUI decodes; scaling and re-encoding remain the app's until a library
  does them (§1).
- No permission dialog for picking on either system; capturing asks for the camera (and microphone) as the system
  requires, and on iOS once to add to Photos.
- A capture is in the user's gallery as well as the app's: a photo taken for a chat stays on the phone, as the camera
  app's own do.

## 4. Options not taken

- **The library through `FileDialog`** (an `extensions` value meaning "photos"): one call fewer, but on a phone a
  different picker with different answers (several kinds, a limit, a capture) under one name.
- **The original HEIC** handed over: smaller and lossless, but nothing in CeeSafe or CeeUI reads it, and the server's
  other clients may not either (MAUI's client converts too).
- **The camera in-app** (AVFoundation, Camera2): a viewfinder of the app's own, needed for scanning and calls; a far
  larger piece, for its own ADR. The system's camera app is what MAUI uses for capture.
- **A `FileProvider` in every APK** for the camera's output: Java in each app, which the `MediaStore` item avoids
  (and the item is the gallery's copy the owner wants kept).

## 5. Tests

- `tools/window_check.py`: scripted `pick` (one, several, cancel) and `capture`; `canCapture` false on the desktop.
- The iOS simulator: `pick` from the simulator's photos (one and several; a HEIC photo comes as JPEG); `capture`
  `Unsupported` there.
- The Android emulator and the owner's Moto: `pick` with the photo picker (one, several, a video), `capture` of a
  photo and a video (the emulator's virtual camera), Back as a cancel.
- The owner's iPhone: `pick` and `capture`, by his hands.
- Marivil's port: a photo and a video sent from the library and from the camera, received by the MAUI app.

Done (2026-11-26):
- `tools/window_check.py`: 69 checks, 0 problems, its new one the scripted calls above.
- The runtime compiled for iOS (arm64, iOS 17), macOS and Android with the corpus's warnings as errors.
- The owner's Moto (moto g35 5G, Android 15), in Marivil's port against marivil.com (Attach's new choices):
  - Photo: the system's photo picker (Google Photos' sheet, several allowed), one photo picked; its copy sent and
    shown in its bubble.
  - Camera, by his hands: the first build gave the camera's app a pending `MediaStore` item, and the camera's app
    ended at its confirm ("Only owner is able to interact with pending/trashed item"); with a new item that is not
    pending, the photo (4.5 MB) was sent and shown, and is in the gallery under `Pictures/Marivil`.
  - Record, by his hands: a video (17.9 MB) sent, and in the gallery under `Movies/Marivil`.
- The owner's iPhone 14 Plus (iOS), by his hands, in Marivil's port against marivil.com: photos picked with
  PHPicker and a photo from the camera were sent and shown, and the capture was added to Photos (iOS asked once).
  He found the camera's photo slow to reach the chat: a full-size photo is uploaded before the message exists, and
  its bubble decodes all 12 megapixels (about 470 ms on an M-series Mac, longer on a phone) — the making smaller that
  §1 left for later.
- Not yet: the iOS simulator; several items at once; a HEIF photo on Android; Android
  before 13 (the content chooser); the storage permission on Android 7.1 to 9.

## 6. Questions for the owner (answered 2026-11-26)

1. **Several at once**: `limit` lets the user choose several photos or videos (recommended: the systems offer it and
   a chat sends several); MAUI's client picks one. Or one only? *Several.*
2. **Photos as JPEG** always (recommended, as MAUI's client does), or the original HEIC/HEIF where the system has one?
   *Always JPEG.*
3. **Android 7.1 to 9** (API 25 to 28), where a `MediaStore` item needs the storage permission: capture asks for
   `WRITE_EXTERNAL_STORAGE` there (recommended; declared up to API 28 only), or capture is `Unsupported` below
   Android 10? *Ask for it.*
4. **A captured photo or video in the gallery**: not kept (recommended, as MAUI's client), or kept? *Kept.*

## 7. Implementation (2026-11-26)

- `lib/std/window.cee`: `MediaKind`, `PickedMedia`, `MediaPicker.pick`, `capture` and `canCapture`, over
  `ceesafe_window_dialog`'s kinds 6 (the library: `name` the limit, `extensions` the kinds) and 7 (the camera), and
  `ceesafe_window_can_capture`; a file's kind is its extension's.
- `runtime/hosted/ceesafe_rt_window.c`: the front maps the kinds to the phones' `cee_native_media` and
  `cee_native_capture`, or to the open dialog with the photos' and videos' extensions elsewhere; a scripted window's
  `dialog` item answers both. iOS: `PHPickerViewController` (its delegate the document picker's, with
  `picker:didFinishPicking:` and the image picker's methods), each result's file copied in its provider's completion
  handler (a global block; one item at a time), a HEIC made JPEG through `UIImage`; `UIImagePickerController` with
  the Info.plist checked first, the capture added with `UIImageWriteToSavedPhotosAlbum` or
  `UISaveVideoAtPathToSavedPhotosAlbum`. Android: `cee_native_media`, `cee_native_capture` and
  `cee_native_can_capture` through `Picker`.
- `runtime/android/org/ceesafe/window/Picker.java`: kinds 6 and 7; the photo picker or the content chooser; a HEIF
  made JPEG with `ImageDecoder`; the camera's app writing to a new `MediaStore` item (Pictures or Movies, under the
  app's name), the permissions asked first through the fragment, the item deleted at a cancel; `canCapture`.
- CeeUI's packer: `--camera`, `--microphone`, `--save-photos`. `spec/type-system.md` §26.13, `spec/api/std.md`.
- A permission refused is `Resources` ("the system refused"): `WindowError` has no `Denied`, and adding one would
  change every exhaustive `match` on it.
