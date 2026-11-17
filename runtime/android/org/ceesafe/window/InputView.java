// The Android window's text editor (ADR 0157): the one Java class of CeeSafe's runtime. tools/gen_android_dex.py
// compiles it to a dex file that runtime/hosted/ceesafe_rt_window.c embeds and loads from memory, so an app has no
// Java of its own. Edit it here, then run the generator.
//
// A view that is a text editor to the system's input method, with an InputConnection over nothing but the text being
// composed (ADR 0141's model): committed text, the composition, and the keyboard's Backspace and Enter go to the
// runtime's native functions, which queue them for the program. All of it runs on the activity's thread.
package org.ceesafe.window;

import android.app.Activity;
import android.content.Context;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import java.nio.charset.StandardCharsets;

public final class InputView extends View {
    /** WindowTextKind's numbers as input types: text, number, decimal, email, URL, password. */
    private static final int[] TYPES = {
        InputType.TYPE_CLASS_TEXT,
        InputType.TYPE_CLASS_NUMBER,
        InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL,
        InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS,
        InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI,
        InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD,
    };

    private static final int COMMIT = 0;
    private static final int COMPOSE = 1;
    private static final int BACKSPACE = 0;
    private static final int ENTER = 1;

    /** The Return key's faces, by WindowReturnKey's number: the key gives Enter whichever it shows. */
    private static final int[] ACTIONS = {
        EditorInfo.IME_ACTION_NONE,
        EditorInfo.IME_ACTION_DONE,
        EditorInfo.IME_ACTION_GO,
        EditorInfo.IME_ACTION_NEXT,
        EditorInfo.IME_ACTION_SEARCH,
        EditorInfo.IME_ACTION_SEND,
    };

    private int inputType = TYPES[0];
    private int action = EditorInfo.IME_ACTION_NONE;
    private boolean shown;
    private String composing = "";

    /** Text the input method committed (kind 0) or is composing (kind 1), as UTF-8. */
    private static native void nativeText(int kind, byte[] utf8);

    /** A key of the keyboard that is no text: Backspace (0) or Enter (1). */
    private static native void nativeKey(int key);

    /** The window's insets may have changed (the keyboard's among them). */
    private static native void nativeInsets();

    public InputView(Context context) {
        super(context);
        setFocusable(true);
        setFocusableInTouchMode(true);
        if (context instanceof Activity) {
            // The window neither shrinks nor pans for the keyboard: the program lays out by its inset.
            ((Activity) context).getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
        }
    }

    /** Text input on or off, and the kind of text: the keyboard shown, hidden, or shown again for a new kind. */
    public void apply(boolean on, int kind) {
        InputMethodManager manager = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        // The kind of text in the low byte, and the Return key's face above it (ADR 0161).
        int text = kind & 0xFF;
        int key = (kind >> 8) & 0xFF;
        int type = TYPES[text < TYPES.length ? text : 0];
        int wanted = ACTIONS[key < ACTIONS.length ? key : 0];
        if (type != inputType || wanted != action) {
            inputType = type;
            action = wanted;
            if (shown) {
                finish();
                manager.restartInput(this);
            }
        }
        if (on) {
            if (!shown) {
                shown = true;
                requestFocus();
                manager.restartInput(this);
            }
            // Also when it was on already: the user may have hidden the keyboard (Back), and asks for it again.
            manager.showSoftInput(this, 0);
        } else if (shown) {
            shown = false;
            composing = "";
            manager.hideSoftInputFromWindow(getWindowToken(), 0);
        }
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo out) {
        out.inputType = inputType;
        out.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI | action;
        out.initialSelStart = 0;
        out.initialSelEnd = 0;
        composing = "";
        return new Connection(this);
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        nativeInsets();
        return super.onApplyWindowInsets(insets);
    }

    /** A hardware keyboard's key that no one took, while text input is on: typed as the soft keyboard's would be. */
    @Override
    public boolean onKeyDown(int code, KeyEvent event) {
        return (shown && key(event)) || super.onKeyDown(code, event);
    }

    /** A key event from the input method or a hardware keyboard: Backspace, Enter, or a character. Whether it was
        one of those. */
    private boolean key(KeyEvent event) {
        int code = event.getKeyCode();
        boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
        if (code == KeyEvent.KEYCODE_DEL) {
            if (!down) {
                return true;
            }
            if (composing.isEmpty()) {
                nativeKey(BACKSPACE);
            } else {
                composing = composing.substring(0, composing.offsetByCodePoints(composing.length(), -1));
                nativeText(COMPOSE, composing.getBytes(StandardCharsets.UTF_8));
            }
            return true;
        }
        if (code == KeyEvent.KEYCODE_ENTER || code == KeyEvent.KEYCODE_NUMPAD_ENTER) {
            if (down) {
                finish();
                nativeKey(ENTER);
            }
            return true;
        }
        int character = event.getUnicodeChar();
        if (character < 0x20) {
            return false;
        }
        if (down) {
            finish();
            nativeText(COMMIT, new String(Character.toChars(character)).getBytes(StandardCharsets.UTF_8));
        }
        return true;
    }

    /** What is being composed is committed as it stands. */
    private void finish() {
        if (!composing.isEmpty()) {
            String text = composing;
            composing = "";
            nativeText(COMMIT, text.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static final class Connection extends BaseInputConnection {
        private final InputView view;

        Connection(InputView view) {
            super(view, false);
            this.view = view;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            view.composing = "";
            nativeText(COMMIT, text.toString().getBytes(StandardCharsets.UTF_8));
            return true;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            view.composing = text.toString();
            nativeText(COMPOSE, view.composing.getBytes(StandardCharsets.UTF_8));
            return true;
        }

        @Override
        public boolean finishComposingText() {
            view.finish();
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            // Nothing surrounds the composition here: the program's text is deleted by its Backspace key.
            for (int k = 0; k < beforeLength; k++) {
                nativeKey(BACKSPACE);
            }
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            view.key(event);
            return true;
        }

        @Override
        public boolean performEditorAction(int action) {
            view.finish();
            nativeKey(ENTER);
            return true;
        }
    }
}
