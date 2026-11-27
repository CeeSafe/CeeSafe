// SPDX-License-Identifier: MIT
// A code scanner (ADR 0274): a full-screen dialog over the activity of the back camera's live picture (Camera2 into
// a TextureView, cropped to fill it) with a Cancel button, whose frames' luminance (an ImageReader's Y plane, about
// 640 by 480) the program reads and decodes itself: Android has no QR decoder of its own. CAMERA is granted first
// (Picker's kind 15). The program's thread calls open, then take, width, height and state each frame, then close; the
// camera runs on a thread of its own, and the dialog on the activity's.

package org.ceesafe.window;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.content.DialogInterface;
import android.graphics.Color;
import android.graphics.ImageFormat;
import android.graphics.Matrix;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Size;
import android.view.Gravity;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;

import java.nio.ByteBuffer;
import java.util.Arrays;

final class Scanner {
    private static final int OPEN = 0;
    private static final int CLOSED = 1;
    private static final int FAILED = 2;

    private final Activity activity;
    private final Object lock = new Object();
    private Dialog dialog;
    private HandlerThread thread;
    private Handler handler;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private ImageReader reader;
    private byte[] frame;   // the newest frame, written by the camera's thread
    private byte[] given;   // the copy the program reads
    private boolean fresh;  // frame has not been taken yet
    private int width;
    private int height;
    private volatile int state = OPEN;

    private Scanner(Activity activity) {
        this.activity = activity;
    }

    /** The scanner, its dialog shown on the activity's thread. */
    static Object open(final Activity activity) {
        final Scanner scanner = new Scanner(activity);
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                scanner.show();
            }
        });
        return scanner;
    }

    /** The newest frame's luminance, width by height, not taken before; null for none new. */
    static byte[] take(Object scanner) {
        Scanner s = (Scanner) scanner;
        synchronized (s.lock) {
            if (!s.fresh || s.frame == null) {
                return null;
            }
            if (s.given == null || s.given.length != s.frame.length) {
                s.given = new byte[s.frame.length];
            }
            System.arraycopy(s.frame, 0, s.given, 0, s.frame.length);
            s.fresh = false;
            return s.given;
        }
    }

    static int width(Object scanner) {
        Scanner s = (Scanner) scanner;
        synchronized (s.lock) {
            return s.width;
        }
    }

    static int height(Object scanner) {
        Scanner s = (Scanner) scanner;
        synchronized (s.lock) {
            return s.height;
        }
    }

    /** 0 open, 1 closed by the user, 2 the camera failed. */
    static int state(Object scanner) {
        return ((Scanner) scanner).state;
    }

    /** Closes the dialog and the camera. */
    static void close(Object scanner) {
        final Scanner s = (Scanner) scanner;
        s.activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (s.dialog != null && s.dialog.isShowing()) {
                    s.dialog.dismiss();
                } else {
                    s.stop();
                }
            }
        });
    }

    private void show() {
        try {
            dialog = new Dialog(activity, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
            FrameLayout root = new FrameLayout(activity);
            root.setBackgroundColor(Color.BLACK);
            final TextureView view = new TextureView(activity);
            root.addView(view, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
            Button cancel = new Button(activity);
            cancel.setText(android.R.string.cancel);
            FrameLayout.LayoutParams place = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
            place.bottomMargin = Math.round(48 * activity.getResources().getDisplayMetrics().density);
            root.addView(cancel, place);
            cancel.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View button) {
                    dialog.dismiss();
                }
            });
            dialog.setContentView(root);
            dialog.setOnDismissListener(new DialogInterface.OnDismissListener() {
                @Override
                public void onDismiss(DialogInterface shown) {
                    if (state == OPEN) {
                        state = CLOSED;
                    }
                    stop();
                }
            });
            view.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
                @Override
                public void onSurfaceTextureAvailable(SurfaceTexture texture, int w, int h) {
                    start(view, texture, w, h);
                }

                @Override
                public void onSurfaceTextureSizeChanged(SurfaceTexture texture, int w, int h) {}

                @Override
                public boolean onSurfaceTextureDestroyed(SurfaceTexture texture) {
                    return true;
                }

                @Override
                public void onSurfaceTextureUpdated(SurfaceTexture texture) {}
            });
            dialog.show();
        } catch (RuntimeException e) {
            state = FAILED;
        }
    }

    /** The back camera (else the first), its preview and a reader of about 640 by 480. */
    private void start(TextureView view, SurfaceTexture texture, int viewW, int viewH) {
        try {
            CameraManager manager = (CameraManager) activity.getSystemService(Context.CAMERA_SERVICE);
            String chosen = null;
            for (String id : manager.getCameraIdList()) {
                Integer facing = manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
                if (chosen == null || (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK)) {
                    chosen = id;
                    if (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK) {
                        break;
                    }
                }
            }
            if (chosen == null) {
                state = FAILED;
                return;
            }
            StreamConfigurationMap map = manager.getCameraCharacteristics(chosen)
                .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size frameSize = nearest(map.getOutputSizes(ImageFormat.YUV_420_888), 640 * 480);
            Size previewSize = nearest(map.getOutputSizes(SurfaceTexture.class), 1280 * 720);
            texture.setDefaultBufferSize(previewSize.getWidth(), previewSize.getHeight());
            crop(view, viewW, viewH, previewSize);
            thread = new HandlerThread("ceesafe-scanner");
            thread.start();
            handler = new Handler(thread.getLooper());
            reader = ImageReader.newInstance(frameSize.getWidth(), frameSize.getHeight(), ImageFormat.YUV_420_888, 2);
            reader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
                @Override
                public void onImageAvailable(ImageReader r) {
                    keep(r);
                }
            }, handler);
            final Surface preview = new Surface(texture);
            manager.openCamera(chosen, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice opened) {
                    camera = opened;
                    begin(preview);
                }

                @Override
                public void onDisconnected(CameraDevice gone) {
                    gone.close();
                    state = FAILED;
                }

                @Override
                public void onError(CameraDevice broken, int error) {
                    broken.close();
                    state = FAILED;
                }
            }, handler);
        } catch (CameraAccessException | RuntimeException e) {
            state = FAILED; // SecurityException without CAMERA, too
        }
    }

    @SuppressWarnings("deprecation")
    private void begin(final Surface preview) {
        try {
            camera.createCaptureSession(Arrays.asList(preview, reader.getSurface()),
                new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraCaptureSession made) {
                        session = made;
                        try {
                            CaptureRequest.Builder request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                            request.addTarget(preview);
                            request.addTarget(reader.getSurface());
                            request.set(CaptureRequest.CONTROL_AF_MODE,
                                CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
                            made.setRepeatingRequest(request.build(), null, handler);
                        } catch (CameraAccessException | RuntimeException e) {
                            state = FAILED;
                        }
                    }

                    @Override
                    public void onConfigureFailed(CameraCaptureSession made) {
                        state = FAILED;
                    }
                }, handler);
        } catch (CameraAccessException | RuntimeException e) {
            state = FAILED;
        }
    }

    /** A frame's Y plane, row by row without the plane's padding, when the program has taken the one before. */
    private void keep(ImageReader r) {
        Image image = r.acquireLatestImage();
        if (image == null) {
            return;
        }
        try {
            synchronized (lock) {
                if (fresh) {
                    return; // the program has not taken the last one: this one is dropped
                }
            }
            Image.Plane y = image.getPlanes()[0];
            int w = image.getWidth();
            int h = image.getHeight();
            int stride = y.getRowStride();
            ByteBuffer buffer = y.getBuffer();
            byte[] into;
            synchronized (lock) {
                into = frame != null && frame.length == w * h ? frame : new byte[w * h];
            }
            for (int row = 0; row < h; row++) {
                buffer.position(row * stride);
                buffer.get(into, row * w, w);
            }
            synchronized (lock) {
                frame = into;
                width = w;
                height = h;
                fresh = true;
            }
        } catch (RuntimeException e) {
            // a frame cut short: the next one
        } finally {
            image.close();
        }
    }

    /** The size whose pixels are nearest `area`. */
    private static Size nearest(Size[] sizes, int area) {
        Size best = sizes[0];
        for (Size s : sizes) {
            if (Math.abs(s.getWidth() * s.getHeight() - area) < Math.abs(best.getWidth() * best.getHeight() - area)) {
                best = s;
            }
        }
        return best;
    }

    /** The preview filling the view without stretching: the camera's landscape picture, turned upright, cropped. */
    private static void crop(TextureView view, int viewW, int viewH, Size preview) {
        boolean portrait = viewH > viewW;
        float contentW = portrait ? preview.getHeight() : preview.getWidth();
        float contentH = portrait ? preview.getWidth() : preview.getHeight();
        float scale = Math.max(viewW / contentW, viewH / contentH);
        Matrix m = new Matrix();
        m.setScale(contentW * scale / viewW, contentH * scale / viewH, viewW / 2f, viewH / 2f);
        view.setTransform(m);
    }

    private void stop() {
        try {
            if (session != null) {
                session.close();
            }
            if (camera != null) {
                camera.close();
            }
            if (reader != null) {
                reader.close();
            }
        } catch (RuntimeException e) {
            // closed already
        }
        session = null;
        camera = null;
        if (thread != null) {
            thread.quitSafely();
            thread = null;
        }
    }
}
