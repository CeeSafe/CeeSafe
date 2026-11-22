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
