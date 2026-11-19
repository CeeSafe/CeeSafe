// Printing a PDF through the system's print dialog (ADR 0178), a class of CeeSafe's runtime compiled into the same
// embedded dex as Picker by tools/gen_android_dex.py. Edit it here, then run the generator.
//
// The PDF is given to PrintManager through a document adapter that copies the file to what the print system asks
// for; the system's dialog shows its pages, the printers and its own "Save as PDF". When the dialog is gone
// (onFinish) and the print system has set the job's state, that state says what the user did: queued or further on
// (Save as PDF's is completed), printed; cancelled, or still only created, not printed; failed, an error. The answer goes to the runtime as a file dialog's does (Picker.nativePicked), since the
// program waits for it as it waits for a picker.
package org.ceesafe.window;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.print.PageRange;
import android.print.PrintAttributes;
import android.print.PrintDocumentAdapter;
import android.print.PrintDocumentInfo;
import android.print.PrintJob;
import android.print.PrintJobInfo;
import android.print.PrintManager;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

final class Printing extends PrintDocumentAdapter {
    private static final int PRINTED = 1;
    private static final int CANCELLED = 0;
    private static final int FAILED = -1;

    private final String file;
    private final String name;
    private PrintJob job;
    private boolean answered;

    private Printing(String file, String name) {
        this.file = file;
        this.name = name;
    }

    /** Opens the print dialog for the PDF at `file`, `title` naming the job. On the activity's thread. */
    static void start(Activity activity, String title, String file) {
        if (!isPdf(file)) {
            Picker.answer(FAILED, null);
            return;
        }
        String name = new File(file).getName();
        String job = title.isEmpty() ? name : title;
        Printing adapter = new Printing(file, name.toLowerCase().endsWith(".pdf") ? name : name + ".pdf");
        PrintManager manager = (PrintManager) activity.getSystemService(Context.PRINT_SERVICE);
        if (manager == null) {
            Picker.answer(FAILED, null);
            return;
        }
        adapter.job = manager.print(job, adapter, null);
    }

    /** Whether the file is there and begins as a PDF does. */
    private static boolean isPdf(String file) {
        byte[] head = new byte[5];
        try (InputStream in = new FileInputStream(file)) {
            int n = 0;
            while (n < head.length) {
                int k = in.read(head, n, head.length - n);
                if (k <= 0) {
                    return false;
                }
                n += k;
            }
        } catch (Exception e) {
            return false;
        }
        return head[0] == '%' && head[1] == 'P' && head[2] == 'D' && head[3] == 'F' && head[4] == '-';
    }

    @Override
    public void onLayout(PrintAttributes old, PrintAttributes now, CancellationSignal cancel, LayoutResultCallback
                         callback, Bundle extras) {
        if (cancel.isCanceled()) {
            callback.onLayoutCancelled();
            return;
        }
        PrintDocumentInfo info = new PrintDocumentInfo.Builder(name)
            .setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT)
            .setPageCount(PrintDocumentInfo.PAGE_COUNT_UNKNOWN)
            .build();
        callback.onLayoutFinished(info, old == null || !old.equals(now));
    }

    @Override
    public void onWrite(PageRange[] pages, ParcelFileDescriptor destination, CancellationSignal cancel,
                        WriteResultCallback callback) {
        // The whole document every time: the print system takes the pages it wants from it.
        try (InputStream in = new FileInputStream(file);
             OutputStream out = new FileOutputStream(destination.getFileDescriptor())) {
            byte[] buffer = new byte[65536];
            for (int n = in.read(buffer); n > 0; n = in.read(buffer)) {
                if (cancel.isCanceled()) {
                    callback.onWriteCancelled();
                    return;
                }
                out.write(buffer, 0, n);
            }
        } catch (Exception e) {
            callback.onWriteFailed(e.getMessage());
            return;
        }
        callback.onWriteFinished(new PageRange[] {PageRange.ALL_PAGES});
    }

    @Override
    public void onFinish() {
        // The dialog is gone, but the job is still "created" for a while (a quarter of a second on Android 16): the
        // print system sets its state after. It is read every tenth of a second until it is set, for SETTLE_MS.
        final Handler handler = new Handler(Looper.getMainLooper());
        final long until = android.os.SystemClock.uptimeMillis() + SETTLE_MS;
        handler.post(new Runnable() {
            @Override
            public void run() {
                if (state() == PrintJobInfo.STATE_CREATED && android.os.SystemClock.uptimeMillis() < until) {
                    handler.postDelayed(this, 100);
                } else {
                    answer();
                }
            }
        });
    }

    /** How long a job may stay "created" after the dialog: past it, it was not printed. */
    private static final long SETTLE_MS = 30000;

    private int state() {
        PrintJobInfo info = job != null ? job.getInfo() : null;
        return info != null ? info.getState() : PrintJobInfo.STATE_CANCELED;
    }

    private void answer() {
        if (answered) {
            return;
        }
        answered = true;
        switch (state()) {
            case PrintJobInfo.STATE_QUEUED:
            case PrintJobInfo.STATE_STARTED:
            case PrintJobInfo.STATE_BLOCKED:
            case PrintJobInfo.STATE_COMPLETED:
                Picker.answer(PRINTED, null);
                break;
            case PrintJobInfo.STATE_FAILED:
                Picker.answer(FAILED, null);
                break;
            default:
                Picker.answer(CANCELLED, null);
                break;
        }
    }
}
