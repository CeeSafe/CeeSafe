# ADR 0272 — Voice notes: recording from the microphone, and playing a sound file

- Status: **Accepted** by the owner 2026-11-27, with §6 answered as recommended (Marivil records while a button is held;
  AAC in an .m4a; the phones and macOS), for Marivil's CeeUI port (its phase 3 lists voice notes). Implemented the same
  day (§7).
- Scope: `lib/std/window.cee` (`AudioRecorder`, `AudioPlayer`), `runtime/hosted/ceesafe_rt_window.c` (the microphone's
  permission as dialog kind 13; `ceesafe_audio_*`), `runtime/android/org/ceesafe/window/Audio.java` (new) and
  `Picker.java`, type-system.md §26.13, `spec/api/std.md`, `tools/window_check.py`. The declarations are the
  packer's (iOS's and macOS's `NSMicrophoneUsageDescription`, CeeUI's `--microphone`; Android's `RECORD_AUDIO`).

## 1. Context

- A messenger records a voice note while a button is held, sends it on release, and plays received ones in their
  bubbles. MAUI records AAC in an MPEG-4 file with Plugin.Maui.Audio; its clients play those.
- iOS: AVFoundation's `AVAudioRecorder` (AAC, metering) and `AVAudioPlayer`, with the audio session set to play and
  record, and `requestRecordPermission:` (the app's `NSMicrophoneUsageDescription`, or iOS ends it).
- macOS: the same classes; the permission is `AVCaptureDevice`'s for audio.
- Android: `MediaRecorder` (MIC, MPEG_4, AAC) and `MediaPlayer`, with `RECORD_AUDIO` asked at run time.
- Windows, Linux and FreeBSD record through APIs a program would need a library for (WASAPI with Media Foundation;
  PipeWire or ALSA); they play a received note through the system's player (`Launcher.openFile`, ADR 0268).

## 2. Decision

```
var note = AudioRecorder.start(mut window, "note.m4a")?;   // the permission, then recording
let loud = note.level();                                   // 0 to 1
let ms = note.stop()?;                                     // or note.cancel()
var player = AudioPlayer.open("note.m4a")?;
player.play();  player.pause();  player.seek(1500);
player.position();  player.duration();  player.playing();
```

- **`AudioRecorder.start(window, path)`** asks the microphone's permission (once; `Resources` when refused) and records
  AAC (44.1 kHz, mono) in an MPEG-4 file at `path`. `level()` is how loud it is now (0 to 1, for a meter); `stop()`
  gives the note's length in milliseconds; `cancel()` deletes the file; a recorder dropped without `stop` is cancelled.
- **`AudioPlayer.open(path)`** plays any file the system plays (AAC, MP3, WAV): `play`, `pause`, `seek(ms)`,
  `position()`, `duration()` and `playing()`; it closes at its drop.
- Windows, Linux and FreeBSD: `Unsupported` for both (§6.3). A scripted window answers the permission with its next
  `dialog` item (none: refused).

## 3. Consequences

- Marivil's port records a voice note while its mic button is held and plays notes in their bubbles, on the phones and
  the Mac; on Windows and Linux a received note opens in the system's player.

## 4. Options not taken

- **Opus in Ogg** (§6.2): smaller at speech's rates, but iOS before 17 does not play it and the MAUI app sends AAC.
- **Recording on Windows and Linux** now: Media Foundation's AAC encoder, or PipeWire with an encoder of CeeSafe's
  own, are larger pieces; they can come when a desktop app records.

## 5. Tests

- `tools/window_check.py`: a refused microphone is `Resources`; a file that is not there cannot be played.
- The Moto and the owner's iPhone, in Marivil's port: a note recorded while the button is held, sent, and played.

## 6. Questions for the owner (answered 2026-11-27)

1. **Marivil's recording**: hold to record (recommended), or tap to start and stop? *Hold to record.*
2. **The format**: AAC in an .m4a (recommended), or Opus in Ogg? *AAC in an .m4a.*
3. **Systems**: the phones and macOS (recommended), or the phones only? *The phones and macOS.*

## 7. Implementation (2026-11-27)

- `lib/std/window.cee`: `AudioRecorder` and `AudioPlayer` (`@noncopy`, given back at their drop) over
  `ceesafe_audio_record_start`, `_level`, `_stop`, `ceesafe_audio_play_open`, `_action`, `_seek` and `_time`; the
  permission is `FileDialog.run`'s kind 13.
- iOS and macOS: one implementation through the Objective-C runtime's own functions (`objc_msgSend`, found with
  dlsym), so it is the same on both: `AVAudioRecorder` with metering (the average power's -50 to 0 dB as 0 to 1),
  `AVAudioPlayer`; the permission's answer a block without captures, the run loop run until it comes; iOS's session
  `PlayAndRecord` with `DefaultToSpeaker` and `AllowBluetooth`; iOS's usage description looked for first.
- Android: `Audio.java` (in the dex): `MediaRecorder` (64 kbit/s), its loudest moment since the last call as the
  level, and `MediaPlayer` with speech's attributes; the recorder's length is read back from the file by a player.
  `Picker` asks `RECORD_AUDIO`.
- The window runtime compiles with `-Wall -Wextra -Wcast-qual -Werror` for macOS, iOS, Android, Linux (gcc 14, clang
  19) and Windows (MinGW-w64 gcc 16).
