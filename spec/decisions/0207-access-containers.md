# ADR 0207 — Containers for screen readers: lists, menus, tab bars and tables

- Status: **Accepted** by the owner, 2026-11-22, the three proposals of §6: a parent with positions the program gives
  or leaves to the runtime; the nine roles; one system per release, ClayWind following each.
- Scope: `lib/std/window.cee` (`WindowAccessNode` gains a parent and a position, `WindowAccessRole` container roles),
  `runtime/hosted/ceesafe_rt_window.c` (each system's accessibility part), type-system.md §26.13,
  `tools/window_check.py`, `tools/atspi_check.py`. No language or compiler change. ADR 0164's "later ADR" (§2.3: "No
  tree. Containers, tables and outlines are a later ADR, which adds a parent to a node without changing what is
  here").

## 1. Context

ADR 0164 gives a window's elements to the screen reader as a flat list. Every system's reader is built around
containers, though, and the flat list shows it:

- **A list's rows are read as buttons or loose items.** VoiceOver says "Invoices" nowhere, never "row 3 of 40",
  and cannot jump past the list; NVDA's list navigation (its quick keys) finds no list; TalkBack does not say "in
  list". ClayWind's `listView`, `dataGrid`, dropdowns and menus all read so (the progress notes of ADR 0164's every
  step, and of ClayWind's web host, list it as "not there").
- **A table is not a table.** ClayWind's data grid reports its column titles as buttons and each row as one
  `ListItem` named by its cells joined; a reader cannot move by column, nor hear a cell's column title.
- **Virtual lists.** ClayWind's lists and grid lay out only the rows on screen. Counting a container's children
  would make a 10,000-row list "40 items"; the program must be able to say how many there are, and which one a row
  is.
- **Menus and tab bars.** A menu's items do not say they are in a menu, nor how many there are; a tab says "tab"
  but not "2 of 4".

## 2. Decision

### 2.1 The node

`WindowAccessNode` gains five fields. A program that gives each 0 has what it has now; struct literals name them, as
they named `caret`, `anchor` and `layer` when ADR 0164 added those:

- **`parent: u64`**: the `id` of the container the element is in, 0 for the window itself. The container comes
  before it in the list (the list stays in reading order, a container before what it holds). A parent that is not
  in the list, or comes later, or is not a container role, is a panic, as an unknown range is (ADR 0164 §2.2).
- **`row: u32`, `column: u32`**: the element's place in its container, from 1: a list's, menu's or tab bar's item
  its `row`; a table's `Row` its `row`; a `Cell` or `ColumnHeader` its `row` and `column`. 0 is "count it": the
  runtime numbers the container's children of that kind in list order.
- **`rows: u32`, `columns: u32`**: on a container, how many items, rows and columns it has, the ones not in the
  list too (a virtual list's rows off the screen). 0 is "count them".

A node is fourteen words to the runtime (ADR 0164 §10 made it eleven): the parent, `row` and `column` in one,
`rows` and `columns` in one.

### 2.2 The roles

`WindowAccessRole` gains **`Group`** (elements that belong together, with a name: a toolbar, a form's section),
**`List`**, **`Menu`**, **`MenuBar`**, **`TabList`**, **`Table`**, **`Row`**, **`Cell`** and **`ColumnHeader`**.
Each has its counterpart on every system:

| Role | macOS | Windows (UI Automation) | Linux (AT-SPI) | iOS | Android |
|---|---|---|---|---|---|
| `Group` | `AXGroup` | Group | `ROLE_PANEL` | a container, `semanticGroup` | a node with children |
| `List` | `AXList` | List | `ROLE_LIST` | a container, `list` | `CollectionInfo` |
| `Menu`, `MenuBar` | `AXMenu`, `AXMenuBar` | Menu, MenuBar | `ROLE_MENU`, `ROLE_MENU_BAR` | a container, `list` | `CollectionInfo` |
| `TabList` | `AXTabGroup` | Tab | `ROLE_PAGE_TAB_LIST` | a container, `list` | `CollectionInfo` |
| `Table` | `AXTable` | DataGrid (Grid, Table) | `ROLE_TABLE` (Table) | a container, `dataTable` | `CollectionInfo` (grid) |
| `Row` | `AXRow` | DataItem | `ROLE_TABLE_ROW` | (the table's rows) | (its cells' row) |
| `Cell`, `ColumnHeader` | `AXCell` | DataItem (GridItem, TableItem), HeaderItem | `ROLE_TABLE_CELL` (TableCell), `ROLE_COLUMN_HEADER` | `UIAccessibilityContainerDataTableCell` | `CollectionItemInfo` |

The items stay what they are (`ListItem`, `MenuItem`, `Tab`, and any role in a `Group` or a `Cell`); what is new is
where they are. A container is focusable and pressable only if the program says so (flags and actions as for any
node); a reader moves into it and out of it itself.

### 2.3 What the runtime does

- It gives each system the tree: the window's children are the nodes whose parent is 0, a container's its own.
- It numbers what the program left at 0 (`row`, `column`, `rows`, `columns`) from the list.
- It answers each system's position questions from the fields: "item 3 of 40", "row 3, column 2", a cell's column
  header (the `ColumnHeader` of that column in the same table).
- Hit tests (ADR 0164 §10's `cee_access_at`) still pick the deepest element at the point, of the highest layer:
  a row before its list.
- Nothing changes for a program that gives no parent: every node is the window's child, as now.

### 2.4 Order of work

Each system is its own step and its own release, as ADR 0164's were; the API is released with the first.

1. **macOS** (and the API): the elements' `accessibilityChildren`, `accessibilityParent`, and the table's rows,
   columns and headers. `window_check` through `AXUIElement`; VoiceOver by the owner.
2. **Windows**: UI Automation's tree navigation (`Navigate` with parent and children), the Grid, GridItem, Table and
   TableItem patterns. NVDA on the owner's laptop.
3. **Linux and FreeBSD**: AT-SPI's `GetChildAtIndex`, `GetIndexInParent`, the Table and TableCell interfaces.
   `atspi_check.py`; Orca on the Fedora VM.
4. **iOS**: `UIAccessibilityContainer` elements with their `accessibilityContainerType`, the data table protocol.
   VoiceOver on the owner's phone.
5. **Android**: the virtual views' parents and children, `CollectionInfo` and `CollectionItemInfo`. TalkBack.
6. **ClayWind** (its own repository, not CeeSafe's): `listView`, `dataGrid` (a `Table` with `ColumnHeader`s and
   `Cell`s), dropdowns, menus, the menu bar and tabs give their containers, as each system arrives; ClayWind's web
   host maps them to ARIA (`list`, `menu`, `tablist`, `grid`, `row`, `gridcell`, `columnheader`, with
   `aria-setsize`, `aria-posinset`, `aria-rowcount`, `aria-rowindex`).

## 3. Alternatives considered

- **Nesting in the list itself** (a container's `childCount` after it, its children next): no new id field, but a
  virtual list's off-screen rows cannot be counted that way, a mistake in one count shifts every later element, and
  the runtime must rebuild the tree to diff it by id anyway.
- **Depth instead of parent** (`level: u32`, each node a child of the nearest shallower one before it): as compact,
  but ids are what the runtime diffs by (ADR 0164 §2.2), and a parent by id keeps a row in its list when the rows
  before it change.
- **Positions always counted by the runtime** (no `row`, `rows`): wrong for every virtual list ClayWind has.
- **Outlines (trees) now**: `TreeItem` with a level and expansion. ClayWind has no tree view yet; a later ADR adds
  the role, the fields here already carry the shape.

## 4. Consequences

- A screen reader hears lists, menus, tab bars and tables as such, with positions and counts, on every system, and
  ClayWind's data grid becomes a table a reader moves through by row and column.
- `WindowAccessNode` is larger (fourteen words); a program that gives the new fields 0 is unchanged, but its
  struct literals must name them.
- Each system's backend grows a tree walk and its table interfaces: the largest part is Windows' and AT-SPI's table
  interfaces.

## 5. What an implementation must show

- `window_check`: a list of three of 40 rows, a menu, a tab bar, and a table of two by three with headers, given
  with and without explicit positions; per system, the reader's API answers the tree, "item 3 of 40" (an explicit
  `rows` over the rows present) and a cell's row, column and header. A parent given later than its child, or not a
  container, is a panic.
- The owner's reader on each system with ClayWind's demo: the list, the data grid and a menu.

## 6. Questions for the owner

1. **The fields**: a `parent` by id, with `row`, `column`, `rows`, `columns` that the program gives or leaves to
   the runtime (proposed), or a parent only, the runtime counting everything (§3: wrong for virtual lists)?
2. **The roles**: `Group`, `List`, `Menu`, `MenuBar`, `TabList`, `Table`, `Row`, `Cell`, `ColumnHeader` (proposed),
   or also `Tree` and `TreeItem` now?
3. **The order**: macOS, Windows, Linux, iOS, Android, one release each, with ClayWind following each (proposed),
   or the three desktops in one release?

## 7. Step 1 as implemented: the API and macOS (2026-11-22)

`lib/std/window.cee`, `runtime/hosted/ceesafe_rt_window.c`, type-system.md §26.13, `tools/window_check.py`,
`tools/atspi_check.py` (its node literal).

- **The list**: fourteen words a node (§2.1). The runtime sorts the ids once (the duplicate check), finds each
  parent by binary search, and refuses a parent not in the list, not before its child, or not a container (-2;
  lib/std panics "an element's parent is not a container before it in the list"). It numbers what is 0 in one pass
  in list order (an item's place among its container's children; a cell's or header's column in its row and its
  row its row's) and then the counts (a container's children, a table's widest row).
- **Systems without containers yet** (`CEE_ACCESS_TREE` undefined: Windows, Linux, iOS, Android until their steps):
  the runtime flattens the list before the backend sees it: containers are left out, a `Row` becomes a `ListItem`
  (a program makes a table's row pressable and selected as it did a list's) and a `ColumnHeader` a `Button` (a
  header sorts); a row with a name holds what is in it, as a control holds its text, so its cells and their
  contents are left out (the row says them, as a list's row did); in a row with none only the cells are. So a program that gives containers reads as it did on
  those systems.
- **macOS**: the roles are `AXGroup`, `AXList`, `AXMenu`, `AXMenuBar`, `AXTabGroup`, `AXTable`, `AXRow` and `AXCell`
  (a column header is a cell of the first row, and the table's `accessibilityColumnHeaderUIElements`). Each element's
  `accessibilityParent` is its container's element or the view; each container's `accessibilityChildren` are its
  children in list order, and the view's (kept, retained) only the top level's. An item's `accessibilityIndex` is
  its `row` less one; a list's and a table's `accessibilityRowCount` their `rows`, a table's
  `accessibilityColumnCount` its `columns`; a cell's `accessibilityRowIndexRange` and
  `accessibilityColumnIndexRange` its place. The tree is laid out again when the list's shape or a place changed.
  Hit tests are unchanged: the latest element of the highest layer at the point, which, children coming after their
  container, is the deepest.
- **Checked** (macOS 26, `window_check --display`, 84 checks): a list of 40 of which three are given at rows 3 to
  5, a menu, a tab bar, a two-by-three table whose first row is its headers, and a group, all numbered by the
  runtime but the list's items; AppKit reports the tree, each element's parent, the places and counts, and the
  table's headers. A parent not in the list, after its child, or not a container: three panics.
- **ClayWind's demo** (ClayWind given containers on the branch's compiler: its listboxes and virtual list as
  lists, its dropdowns and menus as menus, the menu bar, its tabs as a tab list, its data grid as a table of all its
  rows with its titles as column headers), read from another process through `AXUIElement` as a reader reads it:
  the grid a table of 10,001 rows and 4 columns, its rows at their places; the tabs a tab group, each at its place.
  **VoiceOver**, by the owner: "It's working fine".
- **Not yet**: a grid row's cells (ClayWind's `gridCell` has no id of its own yet), so a reader moves by row, not
  by column; the demo's sidebar as a list.

## 8. Step 2 as implemented: Windows (2026-11-23)

`runtime/hosted/ceesafe_rt_window.c` (the UI Automation part), type-system.md §26.13, `tools/window_check.py`.

- **The tree**: `CEE_ACCESS_TREE` for Windows, so the backend has the list as given. A provider's `Navigate` answers
  from the list: its parent is its container's provider (the window's root for the top level), its siblings the
  nodes of the same container before and after it, its first and last child the first and last node it holds; the
  root's children are the top level's. A changed parent or place marks the list's shape changed, and the root's
  children are invalidated, as when an element came or went.
- **Control types**: Group, List, Menu, MenuBar, Tab (a tab list), DataGrid (a table), DataItem (a row, a cell),
  HeaderItem (a column header).
- **Places**: an item of a container (a node with a `row` and no `column`) has `PositionInSet` (its `row`) and
  `SizeOfSet` (its container's `rows`): NVDA's "3 of 40". A selected item's `SelectionContainer` is its container.
- **Tables**: a `Table` has the Grid pattern (`GetItem` by row and column from 0, its `rows` and `columns`) and the
  Table pattern (its column headers, row-major); a `Cell` or `ColumnHeader` the GridItem pattern (its row and column
  from 0, spans of 1, its table) and the TableItem pattern (a cell's column's header). A `Row` is invoked and
  selected as a list's row; a `ColumnHeader` invoked (it sorts).
- **Checked** (`window_check --display` in the desktop session; a C client of UI Automation in another process,
  since a client on the window's own thread waits on the providers it asks): the "tree" window's list with
  "Apple 3 of 40", its menu, tab list and group with their items' places counted, and its table, a grid of 2 by 3
  whose column headers are Name, Size and Kind, each header and cell at its row and column. Windows 11 ARM64 VM
  (LLVM clang) and the owner's x64 laptop (MinGW gcc): 82 checks, 0 problems each.
- **NVDA** on the owner's laptop with ClayWind's demo, driven by a script (clicks at the elements' centres, the keys)
  while NVDA wrote its speech to its log (`--log-level=12` on its command line, its settings untouched): "data
  grid", "Frances Allen, data item, selected, 2 of 10001", the arrows "3 of 10001", "Barbara Liskov … 4 of 10001",
  NVDA's own table key "… 5 of 10001"; "tab control", "Selection, tab, selected, 2 of 5", Right "Tasks … 4 of 5";
  "list", "README.md, 1 of 6", Down "main.c, 2 of 6". On the way it found that ClayWind's lists, grid, tabs and
  listboxes were not reached by the keys at all, so a reader following the focus never came to them; ClayWind
  fixed that (they are Tab stops now, and their chosen row or tab is the focused element). A script's arrows must
  be sent as extended keys: without that flag they are the numeric keypad's, which NVDA takes for its review keys.

## 9. Step 3 as implemented: Linux and FreeBSD (2026-11-23)

`runtime/hosted/ceesafe_rt_window.c` (the AT-SPI part), type-system.md §26.13, `tools/atspi_check.py`.

- **The tree**: `CEE_ACCESS_TREE` for Linux and FreeBSD. An object's `Parent` is its container's object (the
  frame's for the top level); `ChildCount`, `GetChildren` and `GetChildAtIndex` are a container's own children, the
  frame's the top level's; `GetIndexInParent` its index among its container's children. An object's path is still
  its id, so it is the same object wherever it moves. `ChildrenChanged` is sent on the container, with the index
  among its children (a removed element's, from the old list, on its old container).
- **Roles**: panel, list, menu, menu bar, page tab list, table, table row, table cell, table column header.
- **Places**: an item of a container has the object attributes `posinset` and `setsize`, ARIA's, which Orca reads.
- **Tables**: a table has the Table interface (`NRows`, `NColumns`, `GetAccessibleAt` by row and column from 0,
  `GetColumnHeader`, `GetRowAtIndex` and `GetColumnAtIndex` over a row-major index, no selection, no caption);
  its cells and headers the TableCell interface (`Position`, spans of 1, `Table`, `GetRowColumnSpan`, a cell's
  `GetColumnHeaderCells`).
- **Checked** (Fedora 45 VM, GNOME): `atspi_check` reads window_check's tree through pyatspi, as Orca reads it: each
  object's role, name and index in its container, the items' places ("3 of 40" from the program, the rest counted),
  the table's 2 by 3 with its column headers and its six cells found by row and column; 55 checks, 0 problems,
  under Wayland and under X11. `window_check --display` 82 of 82. **Orca** (51.0) with ClayWind's demo, driven with
  GNOME's real pointer and keys (Mutter's RemoteDesktop) while a logging Orca stood in for GNOME's (the screen
  reader setting off meanwhile, and back on after): "table with 10001 rows 4 columns", "Frances Allen", Down
  "Barbara Liskov"; "Selection page tab", Right "Lists page tab"; "List with 6 items", "README.md", Down "main.c".

## 10. Step 4 as implemented: iOS (2026-11-23)

`runtime/hosted/ceesafe_rt_window.c` (the UIKit part), type-system.md §26.13.

- **The tree**: `CEE_ACCESS_TREE` for iOS. A container that holds something is a `UIAccessibilityElement` VoiceOver
  does not stop on (`isAccessibilityElement` NO) whose `accessibilityElements` are its children in list order, and
  it is their `accessibilityContainer`; the view's elements are the top level's. A container that holds nothing is
  not read, but a row or a cell, which is then the element. The tree is laid out again when an element came or
  went, changed its container, or came to hold something or nothing.
- **Frames are in the container's space**: UIKit places a container's child from the container's frame, not from
  the view, so each element is given its frame less its container's origin (found in the Simulator: the cells
  were twice as far down as they are).
- **Container types**: a `Group` is a semantic group (its name is said on the way in), a `List`, `Menu`, `MenuBar`
  and `TabList` a list, a `Table` a data table. A `TabList` also has `UIAccessibilityTraitTabBar`, with which
  UIKit itself gives its tabs their place ("tab, 1 of 2").
- **Tables**: the table's element (a subclass, `CeeSafeAccessTable`) answers `UIAccessibilityContainerDataTable`:
  `accessibilityRowCount` and `accessibilityColumnCount` (its `rows` and `columns`),
  `accessibilityDataTableCellElementForRow:column:` (from 0) and `accessibilityHeaderElementsForColumn:` (its
  `ColumnHeader`s of that column). A `Cell`, a `ColumnHeader` and what a cell holds (`CeeSafeAccessCell`) answer
  `accessibilityRowRange` and `accessibilityColumnRange`. iOS has no rows: a `Row` that holds cells or headers is a
  plain container. A `Row` that holds none is read whole, as a cell of its row as wide as the table and a button as a
  list's row is: one that holds nothing, and one with a name that holds only what says it again (ClayWind's grid rows,
  named by their cells joined, pressed and selected, hold their texts), whose contents are left out, as §7 leaves them
  out where there are no containers. Found with ClayWind's demo: without it each text of a row was a stop of its own
  and the row could not be pressed. An element is kept from list to list while its class is the same; a row that comes
  to hold cells is a new one.
- **No places in lists and menus**: UIKit has no question for "3 of 40" outside a tab bar and a data table (its
  own table views say none either), so a `ListItem`'s and a `MenuItem`'s `row` is not said on iOS.
- **Checked in the iOS Simulator** (iPhone 17, iOS 26.0), in the process, as ADR 0164's step was: a program that gives
  window_check's tree and a table of 10,001 by 4 with two named rows that hold nothing, and a C file that asks UIKit
  (its accessibility loaded as automation loads it, without which the Simulator answers no frames). UIKit reports the
  six top-level containers with their types (list, list, list with the tab bar trait, data table, semantic group, data
  table), each child's container and frame, the tabs' places from UIKit itself (the first and the second of 2), the
  first table 2 by 3 with `Name`, `Size`, `Kind` as its columns' headers and its six elements by row and column, the
  second 10,001 by 4 with `Ada` and `Bob` at rows 2 and 3 across all four columns. After a second list (an item gone,
  the rows moved to 4 and 5, one row now holding a cell) every element is the same object but that row, the places
  follow, and `accessibilityActivate` on a list's item and on a whole row arrives as `Press`.
- **ClayWind's demo in the Simulator** (its desktop host packed by `claywind-pack ios-simulator` with this compiler; a
  library inserted into it asks UIKit for the tree and activates elements by name, as VoiceOver's double tap does):
  the menu bar is a list of its four menus; on Controls, the tabs are a list with the tab bar trait whose five tabs
  UIKit places (1 to 5 of 5), the open one selected; on its Lists tab each listbox is a list of its six rows; on Data,
  the grid is a data table of 10,001 by 4 whose headers are `Order`, `Customer`, `Amount` and `Status`, each visible
  row one element at its row across the four columns, found by row and column; activating "Barbara Liskov" selects
  that row.
- **Not checked yet**: VoiceOver on a phone, which the Simulator does not have: what it says on the way into a list or
  a group, and its reading of a table by row and column.

## 11. Step 5 as implemented: Android (2026-11-23)

`runtime/android/org/ceesafe/window/AccessView.java` and its dex, `runtime/hosted/ceesafe_rt_window.c` (the Android
part), type-system.md §26.13.

- **The tree**: Android was the last system without it, so the runtime no longer flattens a list for any: §7's
  "systems without containers yet", its function and `CEE_ACCESS_TREE` are gone. The packed list carries each node's
  container (its index, -1 for the window), `row`, `column`, `rows` and `columns`. A container is a virtual view whose
  children are what it holds and which is their parent; the view's children are the top level's. A container that
  holds something is not focusable, not clickable, and not under an exploring finger: TalkBack moves through what it
  holds.
- **Widgets**: a `Group` is a `ViewGroup`, a `List`, a `Menu`, a `MenuBar` and a `TabList` a `ListView`, a `Table` a
  `GridView`, a `ColumnHeader` a `Button` that is a heading, a `Cell` that holds nothing a `TextView`. A `TabList` was
  first a `TabWidget`: TalkBack then said no place for a tab, which it says only in a list or a grid. A tab stays a
  `Button`: as `ActionBar.Tab`, the system's own class for one, TalkBack 17 says no kind at all.
- **Counts and places**: `CollectionInfo` on a list and a menu (`rows` by 1), a menu bar and a tab list (1 by `rows`)
  and a table (`rows` by `columns`); `CollectionItemInfo`, from 0, on what a list, menu, menu bar or tab list holds
  (its `row`), and on a `Cell` and a `ColumnHeader` (its `row` and `column`).
- **Rows**, as on iOS (§10), by one function for both: a `Row` that holds cells or headers is a `ViewGroup`; one that
  holds none is a button across the table's columns, and what a named one holds is not packed.
- **Checked on the emulator** (Android 17, API 37, arm64, Google Play image, TalkBack 17.0), the tree read as a screen
  reader reads it (a small program run by `app_process` that connects a `UiAutomation`, as `uiautomator dump` does,
  and prints each node's class, children, `CollectionInfo` and `CollectionItemInfo`), and TalkBack's speech from its
  own verbose log, its cursor moved by `ACTION_ACCESSIBILITY_FOCUS`. A program that gives window_check's tree and a
  10,001 by 4 table of whole rows: the six containers with their children, counts and places (the list 40 by 1 with
  its items at rows 2 to 4, the tab list 1 by 2, the table 2 by 3 with three headings and three cells, the second
  10,001 by 4 with its rows across four columns); TalkBack: "list Fruit, Pear, Button, 4 of 40", "Two, Button, 2 of 2,
  in list Views, 2 items", "Bob, Button, row 4, in grid People, 10001 rows 4 columns"; `ACTION_CLICK` on an item and
  on a whole row arrives as `Press`, and a container refuses it; after a second list the row that came to hold a cell
  is a group of it.
- **ClayWind's demo** (its desktop host packed by `claywind-pack android` with this compiler): the menu bar a list of
  1 by 4, "Edit, Button, 2 of 4"; the tabs 1 by 5, "Tasks, Button, 4 of 5, in list, 5 items"; each listbox 6 by 1,
  "main.c, Button, 2 of 6, in list, 6 items"; the grid 10,001 by 4 with its four headings and its visible rows across
  the columns, "Barbara Liskov, Button, row 4, in grid, 10001 rows 4 columns", "Customer, Button, column heading, row
  1".
- **Not checked**: TalkBack's own gestures (a swipe to the next element, its table navigation), which cannot be
  injected (ADR 0164 §8); a phone. iOS was built and run again with this step's runtime (the Simulator check of §10:
  the same output).

## 12. A named row is read whole on iOS and Android (2026-11-23)

`runtime/hosted/ceesafe_rt_window.c` (`cee_access_whole_row`), type-system.md §26.13.

- **The rule of §10 and §11 was too narrow**: a named row was read whole only when it held no cell. ClayWind's grid
  rows are named (their cells joined), pressed and selected; once they hold `Cell`s, which the desktop readers need to
  move by column, the row would have been only a container on the two phones: not an element, so not pressed, and each
  cell a stop of its own.
- **Now**: on iOS and Android a `Row` with a name is always one element across its table's columns, and what it holds,
  its cells and their contents too, is left out; its name says them, as §7's flattening had it and as these systems'
  own lists read a row. A row with no name is its cells' container (a header row), or, holding nothing, an element. A
  program that wants a reader to move by cell on a phone gives its rows no name. The desktop systems are unchanged: a
  row is a row and holds its cells.
- **Checked**: the Simulator and emulator programs of §10 and §11, whose second list gives a named row a cell: the row
  stays the same element (iOS: the same object, at its row across four columns, found at each column by
  `accessibilityDataTableCellElementForRow:column:`; Android: a button, `CollectionItemInfo` row 4 across 4, and
  `ACTION_CLICK` arrives as `Press`), and the cell is not there; everything else reads as before.

## 13. The phones, by the owner (2026-11-23)

What §10 and §11 left unchecked, with ClayWind's demo built by 0.94.3.

- **Android**, a moto g35 5G (Android 15, TalkBack 17.1), TalkBack running, its cursor moved by
  `ACTION_ACCESSIBILITY_FOCUS` and its speech read from its verbose log: "Controls, Button, 2 of 6, in list Pages, 6
  items"; "Edit, Button, 2 of 4, in list, 4 items"; "Selection, Button, 2 of 5, in list, 5 items"; "main.c, Button, 2
  of 6, in list, 6 items"; "Barbara Liskov, Button, row 4, in grid, 10001 rows 4 columns"; "Customer, Button, column
  heading, row 1". A grid's row and a list's row are selected by TalkBack's click. The tree is the emulator's.
  TalkBack's own gestures are the owner's to make.
- **iOS**, an iPhone 14 Plus, the demo signed and installed on it, VoiceOver by the owner: "I've tested iphone
  successfully."
- **Found**: a second `NativeActivity` started on top of a running one (`am start -n` by component, which the launcher
  does not do) has no access view, so nothing to read: ADR 0164 §8's "a second activity after the first ends". The
  runtime is one window to a program; ClayWind's packer now makes the activity `singleTask`, so such a start comes to
  the activity that is there. A program packed otherwise still has the case.
