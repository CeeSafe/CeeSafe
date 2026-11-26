# ADR 0266 — Making a photo smaller before it is sent, with the system's own codecs

- Status: **Proposed** 2026-11-26, for Marivil's CeeUI port, after the owner found a camera's photo slow to reach the
  chat ("I felt slow in loading from camera to chat window"). Its questions for the owner are §6.
- Scope: `lib/std` (a new `PhotoFile`), `runtime/hosted/ceesafe_rt_image.c` (new: ImageIO on Apple's systems, the
  Android runtime's dex elsewhere through JNI), `runtime/android/org/ceesafe/window/Photos.java` (new),
  type-system.md §26, `tools/image_check.py` (new).

## 1. Context

- A phone's camera makes 12-megapixel photos of 3 to 5 MB. Marivil's MAUI client makes each chat photo at most 1600
  pixels on its longest side, JPEG at quality 0.82, before encrypting and uploading it (about 300 to 500 kB), and a
  640-pixel thumbnail at 0.6 when the result is over 64 kB. The port sends the photo as it came: on the owner's
  iPhone the camera's photo was slow to appear, and every receiver downloads and decodes 12 megapixels.
- Two of three fixes are made (ADR-free): the port shows the message at once while it uploads, and CeeUI decodes a
  JPEG at an eighth of its size for a bubble (about four times faster). This one is the size itself.
- **Nothing in CeeSafe or CeeUI encodes JPEG.** Writing an encoder (and a good scaler) is a large piece; every system
  the phones run has one, fast and often in hardware:
  - Apple (macOS, iOS): ImageIO, `CGImageSourceCreateThumbnailAtIndex` with a largest size (it decodes only what it
    needs, applies the EXIF orientation when asked) and `CGImageDestination` for JPEG at a quality. No UIKit.
  - Android: `BitmapFactory` with `inSampleSize` (decodes at a power-of-two fraction), a scale to the size, the
    orientation from `ExifInterface`, `Bitmap.compress(JPEG, quality)`.
  - Windows: WIC (`IWICBitmapScaler`, the JPEG encoder). Linux and FreeBSD: none in the base system.
- **A photo's metadata** (EXIF: the place it was taken, the camera, the time) goes with the photo when it is sent
  as it is. Re-encoding drops it unless it is copied.

## 2. Decision (proposed)

```
// Result<PhotoInfo, IoError>: the new file's size in pixels
PhotoFile.shrink(from: "IMG_0042.jpg", to: "photo.jpg", longest: 1600, quality: 0.82)
```

- **`PhotoFile.shrink(from, to, longest, quality)`** reads the photo at `from` (JPEG, PNG, HEIC, whatever the system
  decodes), turns it upright (its EXIF orientation applied to the pixels), scales it so its longest side is at most
  `longest` (never larger), and writes it to `to` as a JPEG at `quality` (0 to 1). It answers the new size.
  Metadata is not copied (§6.3). `Unsupported` where the system has no codec (§2.2); `NotFound` or `InvalidData`
  for a file it cannot read as a photo.
- A thumbnail is the same call with other numbers (MAUI's 640 at 0.6).
- **Systems**: ImageIO on macOS and iOS; on Android a runtime class (`Photos`, in the embedded dex) through JNI;
  Windows through WIC (later, as ADR 0146's dialogs came); Linux, FreeBSD and `ceesafec run --interp`:
  `Unsupported`, and a program sends the photo as it is.
- **Marivil's port**: a photo picked or captured is shrunk to MAUI's numbers before it is staged; the original stays
  the user's (in the gallery or the library); a GIF is sent as it is (it may move).

## 3. Consequences

- A camera's photo goes as about a tenth of its bytes: the upload, the receiver's download and decode, and the
  store all shrink by as much. MAUI's clients receive what they send themselves.
- A program on Linux sends photos whole until CeeSafe has an encoder of its own.

## 4. Options not taken

- **A JPEG encoder and scaler in CeeSafe or CeeUI**: one implementation everywhere, the interpreter included, but a
  large piece to make fast and good (a scaler that does not alias, chroma subsampling, an encoder's tables), where
  every phone has one already. It can come later, for Linux.
- **The pickers shrinking as they copy** (a `longest` in `MediaPicker.pick`): one call fewer, but a photo from
  elsewhere (a document, a received file) could not be shrunk, and the picker's job would be two.
- **Keeping the metadata** (§6.3).

## 5. Tests (proposed)

- `tools/image_check.py` on macOS: a 4032 by 3024 JPEG with orientation 6 becomes 1200 by 1600, upright, JPEG, no EXIF;
  a small PNG is not enlarged; a file that is no photo is `InvalidData`; Linux answers `Unsupported`.
- The Moto and the iPhone in Marivil's port: a camera's photo sent as about 1600 pixels and a few hundred kB, shown at
  once, received by the MAUI client upright.

## 6. Questions for the owner

1. **The numbers**: MAUI's, 1600 pixels at 0.82 for a chat photo (recommended), or others?
2. **Where there is no codec** (Linux, FreeBSD; Windows until WIC is done): send the photo whole (recommended for
   now), or write CeeSafe's own encoder first?
3. **Metadata**: dropped (recommended: the place a photo was taken stays with the sender, as most messengers do), or
   kept?
