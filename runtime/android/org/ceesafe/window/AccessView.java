// The Android window's elements for a screen reader (ADR 0164): a view the size of the window that draws nothing and
// takes no touch, whose AccessibilityNodeProvider describes the program's list as virtual views. TalkBack moves
// through them, and what it asks goes to the runtime's native functions, which queue it for the program. All of it
// runs on the activity's thread. tools/gen_android_dex.py compiles it with the runtime's other Java classes.
package org.ceesafe.window;

import android.content.Context;
import android.graphics.Rect;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewParent;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;

public final class AccessView extends View {
    // WindowAccessRole's numbers.
    private static final int TEXT = 0;
    private static final int BUTTON = 1;
    private static final int CHECKBOX = 2;
    private static final int RADIO = 3;
    private static final int TEXT_FIELD = 4;
    private static final int SLIDER = 5;
    private static final int TAB = 6;
    private static final int LIST_ITEM = 7;
    private static final int MENU_ITEM = 8;
    private static final int DROPDOWN = 9;
    private static final int IMAGE = 10;

    /** What each role is to TalkBack: the widget whose name it says. */
    private static final String[] CLASSES = {
        "android.widget.TextView",
        "android.widget.Button",
        "android.widget.CheckBox",
        "android.widget.RadioButton",
        "android.widget.EditText",
        "android.widget.SeekBar",
        "android.widget.Button",
        "android.widget.Button",
        "android.widget.Button",
        "android.widget.Spinner",
        "android.widget.ImageView",
    };

    // WindowAccessNode's flags.
    private static final int CHECKED = 1;
    private static final int SELECTED = 2;
    private static final int DISABLED = 8;
    private static final int SECURE = 32;

    // WindowAccessAction's numbers.
    private static final int PRESS = 0;
    private static final int INCREMENT = 2;
    private static final int DECREMENT = 3;

    private long[] ids = new long[0];
    private int[] virtual = new int[0];
    private int[] roles = new int[0];
    private int[] flags = new int[0];
    private float[] rects = new float[0];   // left, top, width, height of each, in pixels of the view
    private double[] ranges = new double[0]; // min, max, now of each
    private String[] labels = new String[0];
    private String[] values = new String[0];
    private final HashMap<Integer, Integer> indexOf = new HashMap<>();
    private int focused = -1; // the virtual view TalkBack's cursor is on
    private int hovered = -1;
    private boolean asked;
    private final Provider provider = new Provider();

    /** A screen reader asks `action` of element `id`. */
    private static native void nativeAccess(long id, int action);

    /** Something asked for the window's elements. */
    private static native void nativeAccessActive();

    public AccessView(Context context) {
        super(context);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        setWillNotDraw(true);
    }

    /** The program's list, as the runtime packs it: a count, then for each node its id, role, flags, rectangle in
        pixels, range, and its label and value as UTF-8 with their lengths before them. Little-endian. */
    public void update(byte[] packed) {
        ByteBuffer in = ByteBuffer.wrap(packed).order(ByteOrder.LITTLE_ENDIAN);
        int count = in.getInt();
        long[] newIds = new long[count];
        int[] newVirtual = new int[count];
        int[] newRoles = new int[count];
        int[] newFlags = new int[count];
        float[] newRects = new float[count * 4];
        double[] newRanges = new double[count * 3];
        String[] newLabels = new String[count];
        String[] newValues = new String[count];
        HashMap<Integer, Integer> map = new HashMap<>();
        int kept = 0;
        for (int k = 0; k < count; k++) {
            long id = in.getLong();
            int role = in.getInt();
            int flag = in.getInt();
            float x = in.getFloat();
            float y = in.getFloat();
            float w = in.getFloat();
            float h = in.getFloat();
            double min = in.getDouble();
            double max = in.getDouble();
            double now = in.getDouble();
            String label = text(in);
            String value = text(in);
            // A virtual view's id is an int: the element's id folded to 31 bits, so it is the same from list to
            // list. Two of a list that fold to one number: the first is kept.
            int number = (int) (id ^ (id >>> 32)) & 0x7FFFFFFF;
            if (map.containsKey(number)) {
                continue;
            }
            map.put(number, kept);
            newIds[kept] = id;
            newVirtual[kept] = number;
            newRoles[kept] = role >= 0 && role < CLASSES.length ? role : TEXT;
            newFlags[kept] = flag;
            newRects[kept * 4] = x;
            newRects[kept * 4 + 1] = y;
            newRects[kept * 4 + 2] = w;
            newRects[kept * 4 + 3] = h;
            newRanges[kept * 3] = min;
            newRanges[kept * 3 + 1] = max;
            newRanges[kept * 3 + 2] = now;
            newLabels[kept] = label;
            newValues[kept] = value;
            kept++;
        }
        ids = java.util.Arrays.copyOf(newIds, kept);
        virtual = java.util.Arrays.copyOf(newVirtual, kept);
        roles = newRoles;
        flags = newFlags;
        rects = newRects;
        ranges = newRanges;
        labels = newLabels;
        values = newValues;
        indexOf.clear();
        indexOf.putAll(map);
        if (focused != -1 && !indexOf.containsKey(focused)) {
            focused = -1;
        }
        if (hovered != -1 && !indexOf.containsKey(hovered)) {
            hovered = -1;
        }
        if (enabled()) {
            AccessibilityEvent event = AccessibilityEvent.obtain(AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
            event.setContentChangeTypes(AccessibilityEvent.CONTENT_CHANGE_TYPE_SUBTREE);
            onInitializeAccessibilityEvent(event);
            send(event);
        }
    }

    /** Says `utf8` through the screen reader. */
    public void announce(byte[] utf8) {
        announceForAccessibility(new String(utf8, StandardCharsets.UTF_8));
    }

    private static String text(ByteBuffer in) {
        int length = in.getInt();
        byte[] bytes = new byte[length];
        in.get(bytes);
        return new String(bytes, StandardCharsets.UTF_8);
    }

    private boolean enabled() {
        AccessibilityManager manager =
            (AccessibilityManager) getContext().getSystemService(Context.ACCESSIBILITY_SERVICE);
        return manager != null && manager.isEnabled();
    }

    private void send(AccessibilityEvent event) {
        ViewParent parent = getParent();
        if (parent != null) {
            parent.requestSendAccessibilityEvent(this, event);
        }
    }

    private void sendFor(int number, int type) {
        if (!enabled() || !indexOf.containsKey(number)) {
            return;
        }
        AccessibilityEvent event = AccessibilityEvent.obtain(type);
        event.setPackageName(getContext().getPackageName());
        event.setClassName(CLASSES[roles[indexOf.get(number)]]);
        event.setSource(this, number);
        send(event);
    }

    @Override
    public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        return provider;
    }

    /** A finger exploring the screen with TalkBack on comes as hover events: the element under it is entered, and the
        one it left is exited, which is what moves TalkBack's cursor there. */
    @Override
    public boolean dispatchHoverEvent(MotionEvent event) {
        if (!enabled()) {
            return super.dispatchHoverEvent(event);
        }
        int at = -1;
        if (event.getAction() != MotionEvent.ACTION_HOVER_EXIT) {
            float x = event.getX();
            float y = event.getY();
            for (int k = virtual.length - 1; k >= 0; k--) { // the last that holds the point is on top
                if (x >= rects[k * 4] && y >= rects[k * 4 + 1] && x < rects[k * 4] + rects[k * 4 + 2]
                    && y < rects[k * 4 + 1] + rects[k * 4 + 3]) {
                    at = virtual[k];
                    break;
                }
            }
        }
        if (at != hovered) {
            int left = hovered;
            hovered = at;
            if (left != -1) {
                sendFor(left, AccessibilityEvent.TYPE_VIEW_HOVER_EXIT);
            }
            if (at != -1) {
                sendFor(at, AccessibilityEvent.TYPE_VIEW_HOVER_ENTER);
            }
        }
        return true;
    }

    private final class Provider extends AccessibilityNodeProvider {
        @Override
        public AccessibilityNodeInfo createAccessibilityNodeInfo(int number) {
            if (!asked) {
                asked = true;
                nativeAccessActive();
            }
            if (number == HOST_VIEW_ID) {
                AccessibilityNodeInfo host = AccessibilityNodeInfo.obtain(AccessView.this);
                onInitializeAccessibilityNodeInfo(host);
                for (int k = 0; k < virtual.length; k++) {
                    host.addChild(AccessView.this, virtual[k]);
                }
                return host;
            }
            Integer found = indexOf.get(number);
            if (found == null) {
                return null;
            }
            int k = found;
            int role = roles[k];
            int flag = flags[k];
            String label = labels[k];
            String value = (flag & SECURE) != 0 ? "" : values[k];
            AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(AccessView.this, number);
            info.setPackageName(getContext().getPackageName());
            info.setClassName(CLASSES[role]);
            info.setParent(AccessView.this);
            int[] origin = new int[2];
            getLocationOnScreen(origin);
            Rect box = new Rect(Math.round(rects[k * 4]), Math.round(rects[k * 4 + 1]),
                                Math.round(rects[k * 4] + rects[k * 4 + 2]),
                                Math.round(rects[k * 4 + 1] + rects[k * 4 + 3]));
            info.setBoundsInParent(box);
            box.offset(origin[0], origin[1]);
            info.setBoundsInScreen(box);
            info.setVisibleToUser(true);
            info.setEnabled((flag & DISABLED) == 0);
            info.setFocusable(true);
            info.setSelected((flag & SELECTED) != 0);
            // What is said: the name, and after it the value of a field, a dropdown or a slider that gives one.
            if (role == TEXT) {
                info.setText(label);
            } else if (value.isEmpty()) {
                info.setContentDescription(label);
            } else {
                info.setContentDescription(label + ", " + value);
            }
            if (role == TEXT_FIELD) {
                info.setText(value);
                info.setEditable(true);
                info.setPassword((flag & SECURE) != 0);
            }
            if (role == CHECKBOX || role == RADIO) {
                info.setCheckable(true);
                info.setChecked((flag & CHECKED) != 0);
            }
            if (role == SLIDER) {
                info.setRangeInfo(AccessibilityNodeInfo.RangeInfo.obtain(
                    AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT, (float) ranges[k * 3],
                    (float) ranges[k * 3 + 1], (float) ranges[k * 3 + 2]));
                if ((flag & DISABLED) == 0) {
                    info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD);
                    info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_BACKWARD);
                }
            } else if (role != TEXT && role != IMAGE && (flag & DISABLED) == 0) {
                info.setClickable(true);
                info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK);
            }
            if (focused == number) {
                info.setAccessibilityFocused(true);
                info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLEAR_ACCESSIBILITY_FOCUS);
            } else {
                info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_ACCESSIBILITY_FOCUS);
            }
            return info;
        }

        @Override
        public boolean performAction(int number, int action, Bundle arguments) {
            if (number == HOST_VIEW_ID) {
                return AccessView.this.performAccessibilityAction(action, arguments);
            }
            Integer found = indexOf.get(number);
            if (found == null) {
                return false;
            }
            int k = found;
            boolean usable = (flags[k] & DISABLED) == 0;
            if (action == AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS) {
                if (focused == number) {
                    return false;
                }
                focused = number;
                invalidate();
                sendFor(number, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED);
                return true;
            }
            if (action == AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS) {
                if (focused != number) {
                    return false;
                }
                focused = -1;
                invalidate();
                sendFor(number, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
                return true;
            }
            if (action == AccessibilityNodeInfo.ACTION_CLICK && usable && roles[k] != TEXT && roles[k] != IMAGE
                && roles[k] != SLIDER) {
                nativeAccess(ids[k], PRESS);
                sendFor(number, AccessibilityEvent.TYPE_VIEW_CLICKED);
                return true;
            }
            if (roles[k] == SLIDER && usable && action == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD) {
                nativeAccess(ids[k], INCREMENT);
                return true;
            }
            if (roles[k] == SLIDER && usable && action == AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD) {
                nativeAccess(ids[k], DECREMENT);
                return true;
            }
            return false;
        }
    }
}
