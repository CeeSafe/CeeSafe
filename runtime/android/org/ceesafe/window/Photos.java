// Making a photo smaller on Android (ADR 0266): the tenth Java class of CeeSafe's runtime, compiled into the same
// embedded dex by tools/gen_android_dex.py. Edit it here, then run the generator.
//
// The runtime calls it from the program's thread (ceesafe_rt_image.c, attached to the VM for the call). The photo is
// decoded at the largest power-of-two fraction that is still at least `longest` (BitmapFactory's inSampleSize, which
// decodes no more than it needs), scaled and turned upright in one transform (its EXIF orientation, from
// ExifInterface), and written as a JPEG at `quality` thousandths; no metadata goes with it. The answer is the new
// size, {w, h}; null when the file is no photo the system decodes; {-1, -1} when it was read but could not be written.
package org.ceesafe.window;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.media.ExifInterface;

import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;

final class Photos {
    private Photos() {}

    static int[] shrink(String from, String to, int longest, int quality) {
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(from, bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
                return null;
            }
            int sample = 1;
            while (Math.max(bounds.outWidth, bounds.outHeight) / (sample * 2) >= longest) {
                sample *= 2;
            }
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = sample;
            Bitmap decoded = BitmapFactory.decodeFile(from, options);
            if (decoded == null) {
                return null;
            }
            int w = decoded.getWidth();
            int h = decoded.getHeight();
            Matrix turn = new Matrix();
            int big = Math.max(w, h);
            if (big > longest) {
                float scale = (float) longest / big;
                turn.postScale(scale, scale);
            }
            int orientation = ExifInterface.ORIENTATION_NORMAL;
            try {
                orientation = new ExifInterface(from).getAttributeInt(ExifInterface.TAG_ORIENTATION,
                    ExifInterface.ORIENTATION_NORMAL);
            } catch (IOException e) {
                orientation = ExifInterface.ORIENTATION_NORMAL; // no EXIF: as it is stored
            }
            switch (orientation) {
                case ExifInterface.ORIENTATION_FLIP_HORIZONTAL: turn.postScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_180: turn.postRotate(180); break;
                case ExifInterface.ORIENTATION_FLIP_VERTICAL: turn.postScale(1, -1); break;
                case ExifInterface.ORIENTATION_TRANSPOSE: turn.postRotate(90); turn.postScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_90: turn.postRotate(90); break;
                case ExifInterface.ORIENTATION_TRANSVERSE: turn.postRotate(270); turn.postScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_270: turn.postRotate(270); break;
                default: break;
            }
            Bitmap made = turn.isIdentity() ? decoded : Bitmap.createBitmap(decoded, 0, 0, w, h, turn, true);
            int[] size = {made.getWidth(), made.getHeight()};
            try (OutputStream out = new FileOutputStream(to)) {
                if (!made.compress(Bitmap.CompressFormat.JPEG, Math.max(0, Math.min(100, (quality + 5) / 10)), out)) {
                    size = new int[] {-1, -1};
                }
            } catch (IOException e) {
                size = new int[] {-1, -1};
            } finally {
                if (made != decoded) {
                    made.recycle();
                }
                decoded.recycle();
            }
            return size;
        } catch (OutOfMemoryError | RuntimeException e) {
            return null;
        }
    }
}
