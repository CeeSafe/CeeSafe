# ADR 0273 — Video: the system's player over the app, and a video's first frame

- Status: **Accepted** by the owner 2026-11-27, with §6 answered as recommended (the system's own player over the app
  and a first-frame thumbnail; the phones and macOS), for Marivil's CeeUI port (its phase 3 lists video playback).
  Implemented the same day (§7).
- Scope: `lib/std/window.cee` (`VideoPlayer`), `lib/std/photo.cee` (`PhotoFile.videoFrame`),
  `runtime/hosted/ceesafe_rt_window.c` (dialog kind 14), `runtime/hosted/ceesafe_rt_image.c`
  (`ceesafe_image_video_frame`), `runtime/android/org/ceesafe/window/Movie.java` (new), `Picker.java`, `Photos.java`,
  type-system.md §26.13 and §26.24, `spec/api/std.md`, `tools/window_check.py`.

## 1. Context

- A messenger shows a received video as its first frame with a play mark, and plays it full screen with the system's
  controls. MAUI's NativeVideoViewerPage is a page of its own over a native video surface.
- iOS has AVKit's AVPlayerViewController; macOS AVKit's AVPlayerView; Android a VideoView with a MediaController. A
  first frame: AVFoundation's AVAssetImageGenerator, Android's MediaMetadataRetriever.
- Windows and Linux open the file in the system's player already (`Launcher.openFile`, ADR 0268).

## 2. Decision

```
VideoPlayer.show(mut window, "clip.mp4")?;                     // returns when the user closes it
let size = PhotoFile.videoFrame("clip.mp4", "clip.jpg", 640, 0.8)?;
```

- **`VideoPlayer.show(window, path)`** plays the video in the system's own player over the app and returns when the
  user closes it: iOS, AVPlayerViewController presented (Done or a swipe closes it; the audio session is Playback, so
  it is heard with the ringer silenced); macOS, a window of an AVPlayerView named after the file, run modally until it
  is closed; Android, a full-screen dialog of a VideoView and the system's MediaController (Back closes it).
  `Resources` when there is no file or it cannot be played. A dialog of the window (kind 14): a scripted window takes
  its next `dialog` item.
- **`PhotoFile.videoFrame(from, to, longest, quality)`** writes a video's first frame as a JPEG, upright, its longest
  side at most `longest`: `PhotoFile.shrink`'s errors and its JPEG writer.
- Windows, Linux and FreeBSD: `Unsupported` for both.

## 3. Consequences

- Marivil's port shows a video as its first frame and plays it in place of opening another app, on the phones and the
  Mac; on Windows and Linux a video opens in the system's player.

## 4. Options not taken

- **The system's video app** (`Launcher.openFile`): no code, but the user leaves Marivil.
- **Frames drawn by CeeUI**: CeeUI's own controls and a video inside its bubble, but a decoder on each system and
  sound kept in time with the frames: a much larger piece, for later if wanted.

## 5. Tests

- `tools/window_check.py`: a scripted window shows a video; a program writes a missing video's frame (`NotFound` on
  macOS, `Unsupported` where there are none).
- macOS (2026-11-27): a 3 s H.264 MP4 (the Moto's screen recording, 540×1200): its first frame 144×320 at 320 pixels,
  upright; `show` opened the AVPlayerView window playing it, and its close button ended `show`.
- The Moto and the owner's iPhone, in Marivil's port.

## 6. Questions for the owner (answered 2026-11-27)

1. **How a video plays**: the system's player over the app, with a first-frame thumbnail (recommended); the
   thumbnail and the system's video app; or frames drawn by CeeUI? *The system's player and a thumbnail.*
2. **Systems**: the phones and macOS (recommended), or the phones only? *The phones and macOS.*

## 7. Implementation (2026-11-27)

- `lib/std/window.cee`: `VideoPlayer.show` over `FileDialog.run`'s kind 14, the path as its name.
- `ceesafe_rt_window.c`: `cee_native_video` for UIKit (presented, waited on with `cee_ios_wait_dismissed`), AppKit (a
  titled, closable, resizable window, released by the runtime; `runModalForWindow:`, ended by an observer of
  `NSWindowWillCloseNotification` whose block, without captures, pauses the player and calls `stopModal`) and Android
  (`cee_android_pick(14)` → `Picker` → `Movie.show`).
- `ceesafe_rt_image.c`: `ceesafe_image_video_frame`; on Apple's systems AVAssetImageGenerator (the track's transform
  applied, its maximum size `longest`, the frame at 0) and `shrink`'s JPEG writer, now `cee_image_write`; on Android
  `Photos.videoFrame` (MediaMetadataRetriever, OPTION_CLOSEST_SYNC, scaled, Bitmap.compress).
- Both runtimes compile with `-Wall -Wextra -Wcast-qual -Werror` for macOS (arm64 and x86_64), iOS, Android, Linux (gcc
  14, clang 19) and Windows (MinGW-w64 gcc 16).
