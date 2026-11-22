#!/usr/bin/env python3
"""AT-SPI 2 on Linux and FreeBSD (ADR 0164 §3 step 5): a window's elements as a screen reader reads them.

A CeeSafe program opens a real window and gives it one element of each role; this script reads the application from
the accessibility bus with pyatspi (Orca's library), from another process as a reader does: its frame, each element's
role, name and states, a field's text and caret, a slider's value, the actions; then it asks for a press and a focus
through AT-SPI and checks that the program took them (Window.takeAccessAction). With --ping it waits past the
registry's 30 s ping and checks the application is still listed (an unanswered ping hides it).

    python3 tools/atspi_check.py --compiler out/ceesafec [--ping]

Needs a desktop session with its accessibility bus (DISPLAY, DBUS_SESSION_BUS_ADDRESS) and python3-pyatspi.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

PROGRAM = r"""
fn node(id: u64, role: WindowAccessRole, flags: u32, y: f32, label: [u32; 2], value: [u32; 2]) -> WindowAccessNode =>
    WindowAccessNode {
        id: id,
        role: role,
        flags: flags,
        x: 10.0,
        y: y,
        width: 100.0,
        height: 18.0,
        min: 0.0,
        max: if (id == 6) 10.0 else 0.0,
        now: if (id == 6) 4.0 else 0.0,
        labelStart: label[0],
        labelLength: label[1],
        valueStart: value[0],
        valueLength: value[1],
        caret: if (id == 5) 1 else 0,
        anchor: if (id == 5) 2 else 0,
        layer: 0,
        parent: 0,
        row: 0,
        column: 0,
        rows: 0,
        columns: 0,
    };

/// An element of the tree (ADR 0207): its container, its place, its counts.
fn held(id: u64, role: WindowAccessRole, parent: u64, label: [u32; 2], place: [u32; 2], counts: [u32; 2]) ->
    WindowAccessNode {
    var n = node(id, role, 0, round<f32>(id % 100) * 20.0, label, [0, 0]);
    n.x = round<f32>(id / 100) * 10.0;
    n.parent = parent;
    n.row = place[0];
    n.column = place[1];
    n.rows = counts[0];
    n.columns = counts[1];
    return n;
}

/// window_check's tree: a list of 40 rows of which three are given, a menu, a tab list, a table of two by three whose
/// first row is its headers, a group.
fn tree(window: borrow mut Window) {
    let text = "FruitApplePearPlumEditCutCopyViewsOneTwoGridNameSizeKindA1xToolsGo";
    let none = [0u32, 0];
    let nodes = [
        held(100, WindowAccessRole.List, 0, [0, 5], none, [40, 0]),
        held(101, WindowAccessRole.ListItem, 100, [5, 5], [3, 0], none),
        held(102, WindowAccessRole.ListItem, 100, [10, 4], [4, 0], none),
        held(103, WindowAccessRole.ListItem, 100, [14, 4], [5, 0], none),
        held(200, WindowAccessRole.Menu, 0, [18, 4], none, none),
        held(201, WindowAccessRole.MenuItem, 200, [22, 3], none, none),
        held(202, WindowAccessRole.MenuItem, 200, [25, 4], none, none),
        held(300, WindowAccessRole.TabList, 0, [29, 5], none, none),
        held(301, WindowAccessRole.Tab, 300, [34, 3], none, none),
        held(302, WindowAccessRole.Tab, 300, [37, 3], none, none),
        held(400, WindowAccessRole.Table, 0, [40, 4], none, none),
        held(410, WindowAccessRole.Row, 400, [0, 0], none, none),
        held(411, WindowAccessRole.ColumnHeader, 410, [44, 4], none, none),
        held(412, WindowAccessRole.ColumnHeader, 410, [48, 4], none, none),
        held(413, WindowAccessRole.ColumnHeader, 410, [52, 4], none, none),
        held(420, WindowAccessRole.Row, 400, [0, 0], none, none),
        held(421, WindowAccessRole.Cell, 420, [56, 1], none, none),
        held(422, WindowAccessRole.Cell, 420, [57, 1], none, none),
        held(423, WindowAccessRole.Cell, 420, [58, 1], none, none),
        held(500, WindowAccessRole.Group, 0, [59, 5], none, none),
        held(501, WindowAccessRole.Button, 500, [64, 2], none, none),
    ];
    window.setAccessNodes(nodes[..], text.bytes());
}

fn give(window: borrow mut Window, volume: f64, focusField: bool) {
    //          0123456789012345678901234567890123456789012345678901234
    let text = "HeadingSaveAgreeAppleNameVolumeTabRowItemPickIconAlPear";
    var nodes = [
        node(1, WindowAccessRole.Text, 0, 0.0, [0, 7], [0, 0]),
        node(2, WindowAccessRole.Button, if (focusField) 0 else WindowAccessNode.focused(), 20.0, [7, 4], [0, 0]),
        node(3, WindowAccessRole.Checkbox, WindowAccessNode.checked(), 40.0, [11, 5], [0, 0]),
        node(4, WindowAccessRole.Radio, 0, 60.0, [16, 5], [0, 0]),
        node(5, WindowAccessRole.TextField, if (focusField) WindowAccessNode.focused() else 0, 80.0, [21, 4], [49, 2]),
        node(6, WindowAccessRole.Slider, 0, 100.0, [25, 6], [0, 0]),
        node(7, WindowAccessRole.Tab, WindowAccessNode.selected(), 120.0, [31, 3], [0, 0]),
        node(8, WindowAccessRole.ListItem, 0, 140.0, [34, 3], [0, 0]),
        node(9, WindowAccessRole.MenuItem, WindowAccessNode.disabled(), 160.0, [37, 4], [0, 0]),
        node(10, WindowAccessRole.Dropdown, WindowAccessNode.expanded(), 180.0, [41, 4], [51, 4]),
        node(11, WindowAccessRole.Image, 0, 200.0, [45, 4], [0, 0]),
    ];
    nodes[5].now = volume;
    window.setAccessNodes(nodes[..], text.bytes());
}

fn main() -> i32 {
    var window = match (Window.open("atspi check", 320, 240)) {
        Ok(w) => w,
        Err(_) => { Console.writeLine("open failed"); return 1; }
    };
    if (Process.argument(1) == Some("tree")) {
        tree(mut window);
    } else {
        give(mut window, 4.0, true);
    }
    Console.write("active ");
    Console.writeLine(window.accessActive());
    Console.writeLine("ready");
    Console.flush(); // a pipe buffers standard output (ADR 0077); the script waits for this line
    let pixels = [0xFF202020u32; 76800];
    let started = Timer.now();
    while (Timer.now() - started < 90000) {
        loop {
            match (window.nextEvent()) {
                Some(_) => {}
                null => { break; }
            }
        }
        loop {
            match (window.takeAccessAction()) {
                Some(r) => {
                    Console.write("asked ");
                    Console.write(r.id);
                    Console.writeLine(match (r.action) {
                        WindowAccessAction.Press => " press",
                        WindowAccessAction.Focus => " focus",
                        WindowAccessAction.Increment => " increment",
                        WindowAccessAction.Decrement => " decrement",
                    });
                    if (r.id == 2 && r.action == WindowAccessAction.Press && Process.argument(1) != Some("tree")) {
                        // Pressed: the slider moves and the focus goes to Save, for the listener to hear.
                        give(mut window, 7.0, false);
                    }
                    Console.write("active ");
                    Console.writeLine(window.accessActive());
                    Console.flush();
                }
                null => { break; }
            }
        }
        match (window.present(pixels[..], 320, 240)) {
            Ok() => {}
            Err(_) => { Console.writeLine("present failed"); return 1; }
        }
        Window.waitAny(100);
    }
    return 0;
}
"""

# What the elements read as: pyatspi's role name, the name, the states that must be there and those that must not,
# and per role its text, value or action.
EXPECTED = [
    ("label", "Heading", {"showing", "visible", "enabled"}, {"focusable"}),
    ("button", "Save", {"focusable", "enabled"}, {"checked"}),
    ("check box", "Agree", {"checkable", "checked"}, set()),
    ("radio button", "Apple", {"checkable"}, {"checked"}),
    ("entry", "Name", {"editable", "focused", "single line"}, set()),
    ("slider", "Volume", {"focusable"}, set()),
    ("page tab", "Tab", {"selectable", "selected"}, set()),
    ("list item", "Row", {"selectable"}, {"selected"}),
    ("menu item", "Item", set(), {"enabled", "sensitive"}),
    ("combo box", "Pick", {"expandable", "expanded"}, set()),
    ("image", "Icon", set(), {"focusable"}),
]


# The tree as pyatspi reads it (ADR 0207): each object's role, name, index in its container; an item's place in its
# set (the posinset and setsize attributes, "3 of 40"); a table's rows and columns, its column headers and the
# cells it finds by row and column.
TREE_EXPECTED = [
    "list Fruit index=0",
    "  list item Apple index=0 place=3/40", "  list item Pear index=1 place=4/40",
    "  list item Plum index=2 place=5/40",
    "menu Edit index=1",
    "  menu item Cut index=0 place=1/2", "  menu item Copy index=1 place=2/2",
    "page tab list Views index=2",
    "  page tab One index=0 place=1/2", "  page tab Two index=1 place=2/2",
    "table Grid index=3 table=2x3 headers=Name,Size,Kind cells=Name,Size,Kind,A,1,x",
    "  table row  index=0 place=1/2",
    "    table column header Name index=0", "    table column header Size index=1",
    "    table column header Kind index=2",
    "  table row  index=1 place=2/2",
    "    table cell A index=0", "    table cell 1 index=1", "    table cell x index=2",
    "panel Tools index=4",
    "  button Go index=0 place=1/1",
]


def tree_lines(pyatspi, element, depth, lines):
    """What TREE_EXPECTED says of `element`'s children, and of theirs."""
    for k in range(element.childCount):
        child = element.getChildAtIndex(k)
        line = f"{'  ' * depth}{child.getRoleName()} {child.name} index={child.getIndexInParent()}"
        attributes = dict(a.split(":", 1) for a in child.getAttributes() if ":" in a)
        if "posinset" in attributes:
            line += f" place={attributes['posinset']}/{attributes.get('setsize')}"
        if "Table" in pyatspi.listInterfaces(child):
            table = child.queryTable()
            headers = [table.getColumnHeader(c) for c in range(table.nColumns)]
            cells = [table.getAccessibleAt(r, c) for r in range(table.nRows) for c in range(table.nColumns)]
            line += (f" table={table.nRows}x{table.nColumns}" +
                     f" headers={','.join(h.name if h is not None else '-' for h in headers)}" +
                     f" cells={','.join(c.name if c is not None else '-' for c in cells)}")
        if child.parent != element:
            line += " (its parent is not its container)"
        lines.append(line)
        tree_lines(pyatspi, child, depth + 1, lines)
    return lines


def check_tree(exe, expect):
    """The tree program, read from the accessibility bus as Orca reads it."""
    program = subprocess.Popen([exe, "tree"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        program.stdout.readline()
        line = program.stdout.readline().strip()
        expect("the tree's window opens", line == "ready", line)
        if line != "ready":
            return
        import pyatspi
        frame = None
        for _ in range(50):
            apps = [a for a in pyatspi.Registry.getDesktop(0) if a is not None and a.name == "atspicheck"]
            for a in apps:
                if a.childCount > 0 and a.getChildAtIndex(0).childCount > 0:
                    frame = a.getChildAtIndex(0)
            if frame is not None:
                break
            time.sleep(0.2)
        expect("the tree's frame", frame is not None, "not on the desktop")
        if frame is None:
            return
        got = tree_lines(pyatspi, frame, 0, [])
        expect("the containers, places and table (ADR 0207)", got == TREE_EXPECTED,
               "expected\n  " + "\n  ".join(TREE_EXPECTED) + "\ngot\n  " + "\n  ".join(got))
    finally:
        program.terminate()
        program.communicate(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--ping", action="store_true", help="wait past the registry's 30 s ping")
    args = parser.parse_args()
    import importlib.util
    if importlib.util.find_spec("pyatspi") is None:
        print("atspi_check: skipped, no pyatspi (python3-pyatspi)")
        return 0
    problems = []
    checks = 0

    def expect(what, ok, detail=""):
        nonlocal checks
        checks += 1
        if not ok:
            problems.append(f"{what}: {detail}")

    work = tempfile.mkdtemp(prefix="atspi_check_")
    source = os.path.join(work, "atspicheck.cee")
    with open(source, "w") as f:
        f.write(PROGRAM)
    exe = os.path.join(work, "atspicheck")
    b = subprocess.run([os.path.abspath(args.compiler), "build", "--windowed", source, "-o", exe],
                       capture_output=True, text=True, timeout=600)
    if b.returncode != 0:
        print(f"atspi_check: the program does not build: {(b.stdout + b.stderr)[-1500:]}")
        return 1
    program = subprocess.Popen([exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    monitor = None
    try:
        first = program.stdout.readline().strip()
        expect("accessActive before any question", first == "active false", first)
        line = program.stdout.readline().strip()
        expect("the window opens", line == "ready", line + program.stderr.read() if line != "ready" else "")
        if line != "ready":
            return report(problems, checks)
        # Imported once the window is open: pyatspi reads the desktop's applications when it starts, and without a
        # main loop it does not hear of later ones.
        import pyatspi
        # The events, as a listener has them: every signal on the accessibility bus, from dbus-monitor.
        address = subprocess.run(["gdbus", "call", "--session", "--dest", "org.a11y.Bus", "--object-path",
                                  "/org/a11y/bus", "--method", "org.a11y.Bus.GetAddress"],
                                 capture_output=True, text=True).stdout.strip()
        address = address[2:-3] if address.startswith("('") else ""
        log = os.path.join(work, "signals.txt")
        if address:
            monitor = subprocess.Popen(["dbus-monitor", "--address", address, "type='signal'"],
                                       stdout=open(log, "w"), stderr=subprocess.DEVNULL)
            time.sleep(0.5)
        app = None
        for _ in range(50):
            for a in pyatspi.Registry.getDesktop(0):
                if a is not None and a.name == "atspicheck":
                    app = a
            if app is not None and app.childCount > 0 and app.getChildAtIndex(0).childCount > 0:
                break
            time.sleep(0.2)
        expect("the application is on the desktop, named after its program", app is not None,
               [a.name for a in pyatspi.Registry.getDesktop(0) if a is not None])
        if app is None:
            return report(problems, checks)
        expect("the application's role", app.getRoleName() == "application", app.getRoleName())
        frame = app.getChildAtIndex(0)
        expect("its one frame, named by the window's title", app.childCount == 1 and frame.getRoleName() == "frame" and
               frame.name == "atspi check", f"{app.childCount} {frame.getRoleName()} {frame.name!r}")
        expect("the frame's elements, in the list's order", frame.childCount == len(EXPECTED), frame.childCount)
        elements = [frame.getChildAtIndex(k) for k in range(frame.childCount)]
        for element, (role, name, has, lacks) in zip(elements, EXPECTED):
            states = {pyatspi.stateToString(s) for s in element.getState().getStates()}
            expect(f"{name}: role and name", element.getRoleName() == role and element.name == name,
                   f"{element.getRoleName()} {element.name!r}")
            expect(f"{name}: states", has <= states and not (lacks & states), f"{sorted(states)}")
            expect(f"{name}: its parent is the frame", element.parent == frame and element.getIndexInParent() ==
                   elements.index(element), element.getIndexInParent())
        ifaces = {e.name: set(pyatspi.listInterfaces(e)) for e in elements}
        expect("a press for buttons and checkboxes, none for text", "Action" in ifaces["Save"] and
               "Action" in ifaces["Agree"] and "Action" not in ifaces["Heading"], ifaces)
        field = elements[4].queryText()
        expect("the field's text, caret and selection", field.getText(0, -1) == "Al" and field.caretOffset == 1 and
               field.getNSelections() == 1 and tuple(field.getSelection(0)) == (1, 2),
               f"{field.getText(0, -1)!r} {field.caretOffset} {field.getNSelections()}")
        word = field.getTextAtOffset(0, pyatspi.TEXT_BOUNDARY_WORD_START)
        expect("the field's word at its start", tuple(word) == ("Al", 0, 2), word)
        value = elements[5].queryValue()
        expect("the slider's value and range", (value.currentValue, value.minimumValue, value.maximumValue) ==
               (4.0, 0.0, 10.0), (value.currentValue, value.minimumValue, value.maximumValue))
        box = elements[1].queryComponent().getExtents(pyatspi.WINDOW_COORDS)
        expect("Save's box in the window", (box.x, box.y, box.width, box.height) == (10, 20, 100, 18), box)
        hit = frame.queryComponent().getAccessibleAtPoint(box.x + 5, box.y + 5, pyatspi.WINDOW_COORDS)
        expect("the element at a point", hit is not None and hit.name == "Save", hit)
        expect("a focus asked", elements[4].queryComponent().grabFocus(), "grabFocus returned false")
        expect("a press asked", elements[1].queryAction().doAction(0), "doAction returned false")
        time.sleep(1.5)
        if monitor is not None:
            monitor.terminate()
            monitor.wait()
            signals = open(log).read()
            ours = [b for b in signals.split("\nsignal ") if "/org/a11y/atspi/accessible/w" in b]
            value = [b for b in ours if "PropertyChange" in b and "accessible-value" in b and "double 7" in b]
            focused = [b for b in ours if "StateChanged" in b and '"focused"' in b]
            expect("the listener heard the slider's new value", len(value) == 1, f"{len(value)} signals")
            moves = sorted((re.search(r"path=(\S+?);", f).group(1).rsplit("_", 1)[1],
                            re.search(r'string "focused"\s+int32 (\d)', f).group(1)) for f in focused)
            expect("the listener heard the focus move: the field lost it and Save took it",
                   moves == [("2", "1"), ("5", "0")], moves)
            expect("the slider reads its new value", elements[5].queryValue().currentValue == 7.0,
                   elements[5].queryValue().currentValue)
        else:
            expect("dbus-monitor on the accessibility bus", False, "no address")
        if args.ping:
            time.sleep(35)
            listed = [a for a in pyatspi.Registry.getDesktop(0) if a is not None and a.name == "atspicheck"]
            expect("still listed after the registry's ping", len(listed) == 1 and
                   not listed[0].getState().contains(pyatspi.STATE_DEFUNCT), "hidden as unresponsive")
    finally:
        if monitor is not None and monitor.poll() is None:
            monitor.terminate()
        program.terminate()
        out, err = program.communicate(timeout=10)
    asked = [l for l in out.splitlines() if l.startswith("asked")]
    expect("the program took the focus and the press", asked == ["asked 5 focus", "asked 2 press"], asked)
    expect("accessActive once asked", "active true" in out.splitlines(), out.splitlines()[:4])
    check_tree(exe, expect)
    return report(problems, checks)


def report(problems, checks):
    for p in problems:
        print(f"FAIL {p}")
    print(f"atspi_check: {checks} checks: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
