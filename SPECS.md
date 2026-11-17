# tcl-csvvw — Specification

A Total Commander Lister plugin (64-bit) that shows CSV/TSV files as a fast, read-only table.

- **Status:** v1 spec, drafted from the requirements interview on 2026-11-17
- **Repo:** https://github.com/Civ01/tcl-csvvw
- **Binary name:** `tcl-csvvw.wlx64`
- **Size target:** < 1 MB, with speed taking priority over size (decided 2026-11-17; v1 is ~525 KB with `/O2`). The current best third-party plugin is ~2 MB.
- **Basis:** the plugin plumbing, settings framework, dark mode, build and CI are extracted from [tcl-syntaxhl](https://github.com/Civ01/tcl-syntaxhl). The view (RichEdit there) is replaced by a custom owner-drawn grid. No third-party CSV or grid library is used.

---

## 1. Goals and non-goals

### 1.1 Goals (v1)

1. Render CSV/TSV as a table in Lister (F3) and Quick View (Ctrl+Q). Opening should be near-instant at 5,000+ rows and smooth up to the 100 MB file limit.
2. Make the font face and size configurable.
3. Auto-size columns to fit their content, with a width cap. This is configurable on/off.
4. Zoom with Ctrl+wheel, with an option to remember the last zoom.
5. Select by cell, row, column or rectangle, following the mouse and keyboard. Copy as TSV, or optionally in CSV format.
6. Sort by column, cycling ascending → descending → original order, with automatic type detection.
7. Search text through TC's own Find dialog (F7, F3 / Shift+F3).
8. Auto-detect the delimiter, encoding and header row. Each can be overridden per file from the right-click menu.
9. Configure everything through `tcl-csvvw.ini`, a Settings window and quick toggles in the right-click menu.

### 1.2 Non-goals (v1)

- Editing, saving, filtering, formulas or column statistics.
- Disjoint (Ctrl+click) multi-range selection. There is one rectangular selection.
- Selecting part of the text inside a cell. The cell is the smallest selection unit.
- Column hide, reorder or freeze beyond the first column.
- Printing (`ListPrint`).
- A 32-bit (`.wlx`) build, Windows older than 10, and Excel/ODS files.
- Files larger than `max_file_size` (default 100 MB). These get a notice (§7.4).

---

## 2. Platform and build

Same as tcl-syntaxhl unless noted.

| Item | Decision |
|---|---|
| Language | C++20, MSVC (VS2022, v143 toolset) |
| Target | Windows 10+, x64 only, TC 9.x+ (dark mode needs TC 11+) |
| Build | CMake ≥ 3.20. Static CRT (`/MT`). Output `tcl-csvvw.wlx64` with exports via `.def` |
| Rendering | GDI (`ExtTextOutW` + `ETO_CLIPPED`), double-buffered. No RichEdit, no DirectWrite |
| Runtime deps | None. Tests use the vendored doctest |

### 2.1 Portability split

- **Core** (`src/core`, `src/csv`) is portable C++ with no `<windows.h>`. It covers the parser, row index, sniffing, header heuristic, type detection, sort keys and comparators, search matcher, clipboard serialisers, the config model and the INI document. It is unit-tested on Linux (ASan/UBSan) and MSVC.
- **Platform** (`src/plugin`, `src/view`, `src/ui`) holds the TC exports, grid window, menus, Settings dialog and dark mode. Locale-aware text comparison (`CompareStringEx`) and ANSI decoding are injected into the core as callbacks, as `TextDecoder` already does in tcl-syntaxhl.

### 2.2 Reused from tcl-syntaxhl (milestone M0)

| Kept (renamed or trimmed) | Dropped |
|---|---|
| `plugin/exports.cpp`, `listplug.h`, `.def`, `WinUtil`, `DebugLog` | lexers, regex backend, `.rules`, theme files and theme editor |
| `core/IniDocument` (comment-preserving writes), `Config`, `OptionSchema`, `Settings`, `TextDecoder` (adapted to output UTF-8, §5.1), `TextUtil` | `Highlighter`, RichEdit code, `LazyModel`, `GotoLineDialog` |
| `ui/ConfigDialog` + page framework, `ui/DarkMode`, `view/InfoBar` (notices), `resources.rc`, `version.h.in` | |
| `CMakeLists.txt` layout, `ci.yml` (MSVC + Linux sanitizers + MinGW cross), `release.yml` (tag → zip), `cmake/mingw-w64-x86_64.cmake`, `tools/plugin_smoke` (rewritten for the grid), `tests/third_party/doctest.h` | |

### 2.3 Distribution package

```
tcl-csvvw.wlx64
tcl-csvvw.ini      ; default config, fully commented
pluginst.inf       ; description=CSV/TSV table viewer, type=wlx, file=tcl-csvvw.wlx, defaultdir=tcl-csvvw
README.md
```

---

## 3. Total Commander Lister API

| Export | Behaviour |
|---|---|
| `ListGetDetectString` | Built from `[General] extensions` (default `csv,tsv,tab`), e.g. `EXT="CSV" \| EXT="TSV" \| EXT="TAB"`. TC caches it at install time, so the README explains how to refresh it. |
| `ListSetDefaultParams` | Stores the interface version. The INI path comes from the DLL location. |
| `ListLoadW` / `ListLoad` | Returns `NULL` for unmapped extensions unless `lcp_forceshow` is set. Otherwise it reads, indexes and shows the file (§7). |
| `ListLoadNextW` / `ListLoadNext` | Reuses the window. Per-file overrides (delimiter, encoding, header, sort, selection) reset; view toggles and zoom persist. |
| `ListCloseWindow` | Frees all per-window state and saves the zoom if `remember_zoom=true`. |
| `ListSearchTextW` / `ListSearchText` | §11. |
| `ListSendCommand` | `lc_copy` copies the selection as TSV. `lc_selectall` selects all. `lc_newparams` applies `lcp_darkmode`. `lc_setpercent` scrolls vertically to that percent. |
| Not exported | `ListPrint`, `ListGetPreviewBitmap`, `ListSearchDialog`, `ListNotificationReceived`. |

**Integration rules.** These are carried over unchanged from tcl-syntaxhl §3.1/§16:
- Percent reporting through `itm_percent`.
- Unhandled keys are *posted* to the container window (Esc, N/P, 1–7, F-keys…). `WM_CHAR` is not forwarded.
- No focus stealing in Quick View (parent class ≠ `TLister`).
- All state is per window.
- The INI is re-read on every load.
- Exceptions never cross the C ABI.
- No beeps.

---

## 4. Architecture

```
TC ──► plugin/exports.cpp
            │
            ▼
      view/ListerWindow (container)
            ├─► view/GridView      owner-drawn grid: header, gutter, cells, scroll, selection, hit-test
            ├─► view/StatusBar     bottom strip (toggleable)
            ├─► view/InfoBar       top notice strip (oversized file, decode problems)
            └─► ui/ContextMenu, ui/ConfigDialog (+ pages), ui/DarkMode
      core:
            csv/Loader        bytes → UTF-8 buffer (TextDecoder) → Sniffer → RowIndex
            csv/Sniffer       delimiter + header heuristic
            csv/RowIndex      row start offsets; per-row field splitting on demand
            csv/ColumnStats   one pass at load: max columns, type votes, auto-size candidates
            csv/SortEngine    per-column keys, permutation array
            csv/Search        cell-by-cell matcher in display order
            csv/Clipboard     TSV / CSV serialisers
            core/Config, Settings, OptionSchema, IniDocument
```

`Table` (core) is the model the view reads from:
- `rowCount()`, `colCount()`, `cell(dataRow, col) -> std::string_view`. The returned text is unescaped, and capped or uncapped as requested.
- `displayToData` permutation (identity until sorted).
- `hasHeader`, `delimiter`, `encoding`.

---

## 5. CSV parsing

### 5.1 Input and encoding

- The whole file is read into memory with `ReadFile` (≤ `max_file_size`). No memory mapping, so the file is not held open or locked while viewing.
- Encoding: BOM detection (UTF-8, UTF-16 LE/BE), then UTF-8 validation, otherwise the ANSI code page.
- **Internally everything is UTF-8.** UTF-16 and ANSI input are converted to UTF-8 once at load, so the parser only ever scans bytes and every delimiter and quote is ASCII.
- Cells are converted to UTF-16 only when they are drawn, measured, compared or copied.
- The encoding can be overridden from the right-click menu: Auto / UTF-8 / UTF-16 LE / UTF-16 BE / ANSI. This re-runs the load for the current file only.

### 5.2 Dialect

RFC 4180 plus common real-world variants:
- Delimiter is one of `,` `;` Tab `|`. The quote character is always `"`, escaped as `""`.
- Quoted fields may contain delimiters and line breaks.
- Line endings: CRLF, LF or lone CR, mixed in one file.
- Lenient handling:
  - A stray `"` inside an unquoted field is literal text.
  - After a closing quote, text up to the next delimiter is appended verbatim.
  - An unterminated quote at EOF ends the field at EOF.
  - None of these are errors and none abort the load.
- A trailing empty line at EOF is not a row. Blank lines in the middle are rows with one empty cell, shown padded.
- NUL bytes are shown as a space.

### 5.3 Delimiter detection

- Sample the first 64 KB (whole lines, quote-aware).
- For each candidate, compute the field count per line. The score is the consistency (share of lines with the modal count) × whether the modal count is > 1.
- The highest score wins. On a tie: Tab for `.tsv`/`.tab`, otherwise the order `,` `;` Tab `|`.
- If no candidate gives > 1 field, the file is a single column.
- Override from the right-click menu: Auto / `,` / `;` / Tab / `|`. It applies to the current file only. The INI `delimiter=auto|,|;|tab|pipe` sets the default.

### 5.4 Row index

- One quote-aware pass records the byte offset where each row starts (`std::vector<uint32_t>`).
- In the same pass, `ColumnStats` splits every row. It records the maximum field count, which becomes `colCount`.
- **Ragged rows** are padded with empty cells up to `colCount`.
- Fields are split per row on demand: offset and length, plus a flag for "needs `""` unescape".
- An LRU cache holds the split and decoded fields of recently drawn rows, roughly two screens' worth.

### 5.5 Cell cap

- `max_cell_chars` (default **32768** characters).
- The cap applies to drawing and auto-size measurement only. A capped cell is drawn with a trailing `…`.
- **Search and copy always use the full cell text.**
- Range 256…1048576. The INI accepts K/M suffixes.

### 5.6 Header detection

The heuristic compares row 1 with the column statistics of all following rows (collected in the load pass anyway):

- **Header = yes (definitive)** when all of these hold:
  - Every non-empty cell in row 1 is non-numeric and non-date.
  - At least one column is "typed" (≥ 90% of its non-empty sample cells are numeric or date), and row 1's cell in that column is not of that type.
- **Header = no (definitive)** when row 1 has a numeric or date cell in a column that is typed the same way.
- **Otherwise inconclusive.** For example, an all-text table falls back to `header_default` from the INI (default `true`).
- The right-click menu has a checkbox, **"First row is header"**.
  - It shows the effective state for the current file.
  - Toggling it applies immediately to the current file and saves `header_default` to the INI.
- `header_mode=auto|on|off` (Settings window) can force the state for every file; the default is `auto`.

With a header, row 1 becomes the column header captions and is excluded from sorting and the data row count. Without one, captions are spreadsheet letters A, B, … Z, AA, ….

---

## 6. Grid view

### 6.1 Layout

```
┌──────┬───────────────┬────────────┬──────────
│  #   │ Name ▲        │ Price      │ …         ← column header (frozen at top)
├──────┼───────────────┼────────────┼──────────
│    1 │ …             │       12.5 │
│    2 │ …             │            │
└──────┴───────────────┴────────────┴──────────
 12,345 rows × 18 cols │ ; │ UTF-8 │ Header: auto │ Sorted: Price ▼ │ Sel: 3 × 5   ← status bar
```

- **Row number gutter** (toggle `show_row_numbers`, default on). Shows the *original* data row number, so numbers stay meaningful after sorting. Clicking it selects the row.
- **Frozen first column** (toggle `freeze_first_column`, default off). Column 1 stays visible during horizontal scrolling. A thicker divider marks the freeze.
- **Status bar** (toggle `show_status_bar`, default on). Shows rows × columns, delimiter, encoding, header state (auto/forced), sort state and selection size.
- **Number alignment:** cells in numeric-typed columns are right-aligned; everything else is left-aligned. The header is left-aligned. Configurable with `align_numbers_right`.
- **Zebra rows** (`zebra_rows`, default on) and horizontal and vertical grid lines.

### 6.2 Rendering

- Only the visible row and column range is drawn, into a back buffer, then blitted to the window.
- Each cell is clipped to its rectangle.
- One `HFONT` per zoom level (plus a bold variant for the header).
- The text of visible cells is converted from UTF-8 to UTF-16 through the row cache.
- **Line break glyph (all modes):** a line break that is part of a cell's own text (CRLF, LF or CR inside a quoted field) is **always** drawn as a dim `↵`. In multi-line mode, and wherever text wraps, the `↵` is drawn at the end of the line it ends. This tells the cell's own breaks apart from soft wraps (§6.3.1), which get no glyph. If the table font has no `↵` glyph, the `linebreak_fallback` character (default `¶`) is used instead. Tabs are drawn as a space.
- **Single-line mode** (default):
  - The row height is fixed at the font height plus padding, except for rows grown by last-column wrapping (§6.3.1).
  - A cell's lines are drawn inline: `first↵second↵third`.
  - Hovering a cell that contains line breaks or is truncated shows a tooltip with its text, capped.
- **Multi-line mode** (right-click toggle **"Show line breaks in cells"**, saved as `multiline_cells`, default off):
  - Each of a cell's own lines is drawn on its own visual line, ending with `↵`.
  - Row height = the most lines in any of the row's cells, capped at `max_cell_lines` (default 8), with `…` on overflow.
- **Variable row heights:** once any row can be taller than one line (multi-line mode or last-column wrapping), rows are positioned with a prefix-sum array in display order. The array is rebuilt on sort, zoom, toggle and wrap-width changes, and scrolling becomes pixel-based. Otherwise the fast path applies: a fixed row height and whole-row scrolling.
- **High DPI:** per-monitor DPI aware. All pixel settings are at 100% zoom and 96 DPI, and are scaled.

### 6.3 Columns and auto-size

- Auto-size is on by default (`auto_size_columns`). It is also a right-click toggle.
- **Fit width** = the widest of the header caption and the column's content, clamped to `[min_column_width, max_auto_width]`. Defaults are 40 px and **400 px**.
- **Measuring cost:** `ColumnStats` keeps the 8 longest cells (by character count, capped) per column. Only those, plus the caption, are measured with `GetTextExtentPoint32W`. This is exact enough for proportional fonts and costs O(columns) per measure.
- Widths are stored at zoom 100% and scaled proportionally on zoom, with no re-measuring.
- When auto-size is off, columns get `default_column_width` (100 px).
- **Manual resize:** dragging a header divider always works. Double-clicking a divider fits that column, ignoring the `max_auto_width` cap (spreadsheet behaviour) but not the hard limit `max_fit_width` (default **4000 px**).
- Header right-click offers *Fit column* and *Fit all columns*.
- Manual widths last for the current file only.

#### 6.3.1 Last column mode

Right-click → **Last column ▸** has two checkable items. They are mutually exclusive, and both may be off:

| Item | INI (`last_column=`) | Behaviour |
|---|---|---|
| *(both off)* | `normal` (default) | The last column is sized like every other column: capped at `max_auto_width`, with `…`. |
| **Fully-fit last column** | `fit` | The last column is as wide as its widest cell. It ignores `max_auto_width` but is limited by `max_fit_width` (4000 px). It applies whether or not auto-size is on. |
| **Wrap text in last column** | `wrap` | Described below. |

Checking one item unchecks the other; checking the active item again turns it off. The choice is saved to the INI immediately.

**Wrap text in last column:**
- **Wrap width** = max(the column's normal width, the free space from the column's left edge to the window's right edge). The normal width is its auto-size width capped at `max_auto_width`, or `default_column_width` or a manually dragged width.
  - The column fills the window when there is room.
  - Resizing the window re-wraps.
  - When the content is narrower than the wrap width, the column takes its content width and nothing wraps.
- **Wrapping:** word wrap at spaces and after `-` `/` `,` `;`. A single word longer than the wrap width is broken between characters.
  - At most `max_cell_lines` visual lines (default 8); the last visible line ends with `…` when text is cut.
  - The row height grows to fit, using the variable row height path (§6.2).
- **A last-column cell that contains its own line breaks is never soft-wrapped.**
  - In single-line mode it stays one line (`a↵b↵c`), truncated with `…` at the wrap width.
  - In multi-line mode it is split only at its own breaks, and each line is truncated with `…`.
- **Height computation:**
  - A cell needs wrapping only if its width can exceed the wrap width. Cells that obviously fit, judged by character count × the font's maximum character width, cost nothing.
  - The others are measured with `GetTextExtentExPointW`.
  - Files with up to 200,000 rows are measured exactly when the mode is turned on, and on zoom, font or width changes.
  - For larger files, heights start as estimates from the average character width and are corrected as rows are drawn. The top visible row stays anchored, so the view does not jump.
- **Copy and search** are unaffected: they work on the cell text, not on the wrapped lines.

### 6.4 Font and zoom

- `font_face` defaults to `Segoe UI` and `font_size` to `10`. Any installed font can be chosen in Settings.
- **Ctrl+wheel** zooms in 10% steps over 50–400%.
- Keyboard: Ctrl+`+` and Ctrl+`-` zoom; Ctrl+0 resets to 100%. The right-click menu also has *Reset zoom*.
- With `remember_zoom=true` (default), the zoom is written to `zoom=` in the INI on close or file change. This only happens when the zoom changed.

### 6.5 Colors / dark mode

- `theme=auto|light|dark`. `auto` follows TC's `lcp_darkmode`.
- Built-in light and dark palettes. Each color can be overridden in `[ColorsLight]` / `[ColorsDark]` as `#RRGGBB`; an empty value means built-in.
- Keys:
  - `background`, `text`
  - `grid_lines`, `header_background`, `header_text`, `alt_row_background`
  - `selection_background`, `selection_text`, `current_cell_border`
  - `search_hit_background`
  - `gutter_background`, `gutter_text`
  - `frozen_divider`, `glyph` (the `↵` and `…` glyphs)
- Dark mode for the context menu, tooltips, scrollbars and Settings window comes through `ui/DarkMode`, as in tcl-syntaxhl.

---

## 7. Loading

1. Read the file. If it is larger than `max_file_size` (default **100M**), do not parse it; see §7.4.
2. Decode to UTF-8 (§5.1).
3. Sniff the delimiter and dialect (§5.3).
4. Build the row index and `ColumnStats` in one pass (§5.4).
5. Detect the header (§5.6), compute column types (§9.1) and fit column widths (§6.3).
6. Show the grid.

Loading is synchronous. A wait cursor shows during loads that take more than ~150 ms.

### 7.4 Oversized and failed files

- **Oversized:** the plugin claims the file and shows an InfoBar notice, "File is 250 MB; tcl-csvvw shows files up to 100 MB (max_file_size). Press 1–3 to switch to another viewer." The Lister hotkeys keep working.
- **Read or decode failure:** returns `NULL`, so TC falls back to another viewer.

---

## 8. Selection, keyboard and clipboard

### 8.1 Model

- The selection is one rectangle, defined by an **anchor cell** and the **current cell**, in display coordinates.
- Whole-row selection, whole-column selection and select-all are special cases of that rectangle.
- The current cell has a focus border.

### 8.2 Mouse

| Action | Result |
|---|---|
| Click a cell | Select that cell (anchor = current). |
| Drag across cells | Extend the rectangle; auto-scrolls at the edges. |
| Shift+click a cell | Extend from the anchor. |
| Click or drag in the gutter; Shift+click | Select rows / extend the row range. |
| **Click a header** (press and release, no movement) | Sort that column: asc → desc → original (§9). |
| **Ctrl+click a header** | Select that column. |
| Ctrl+Shift+click a header, or drag across headers | Extend the column selection. |
| Drag or double-click a header divider | Resize / fit the column (§6.3). |
| Click the top-left corner | Select all. |
| Ctrl+wheel | Zoom (§6.4). |
| Wheel / Shift+wheel | Vertical / horizontal scroll, using `SPI_GETWHEELSCROLLLINES`, with sub-notch accumulation (as in tcl-syntaxhl). |

### 8.3 Keyboard (spreadsheet-style)

- Arrows move the current cell; Shift+arrows extend the selection.
- Home and End go to the first and last column of the row; Ctrl+Home and Ctrl+End go to the first and last cell.
- PgUp and PgDn move one screen.
- Ctrl+A selects all, Ctrl+C copies TSV and Ctrl+Shift+C copies as CSV.
- Navigation at the table edges is silent, with no beep.
- All other keys are forwarded to Lister, including Esc, N/P, 1–7 and F-keys. F7 / F3 / Shift+F3 reach TC, which calls `ListSearchText`.

### 8.4 Clipboard

The text goes on the clipboard as `CF_UNICODETEXT`.

- **Copy (Ctrl+C, `lc_copy`):** TSV in display order. Cells are joined with Tab, and rows end with CRLF. A cell containing Tab, CR, LF or `"` is quoted Excel-style (`"..."` with `""`).
- **A single cell** is copied as its raw text, with no quoting.
- **Copy as CSV** (menu, Ctrl+Shift+C): the source delimiter with RFC 4180 quoting, applied only where needed.
- **Copy with header** (menu): the header captions of the selected columns are prepended. This is only enabled when the file has a header.
- Copies always use the full, uncapped cell text.

---

## 9. Sorting

### 9.1 Column type detection

Types are worked out in the load pass, over all non-empty data cells:

| Type | Accepted forms |
|---|---|
| **number** | Optional sign, digits, optional `.` decimal, optional exponent. Also `,` as the decimal separator when the column contains no `.` decimals. Thousands separators (space, NBSP, `'`) and a trailing `%` are allowed. |
| **date** | ISO `YYYY-MM-DD` with an optional `[T ]hh:mm[:ss[.fff]]`; `DD.MM.YYYY` with an optional time. US `MM/DD/YYYY` is not recognised, because it is ambiguous. |
| **text** | Anything else. |

A column is number or date when at least `type_threshold` (default 90%) of its non-empty cells parse as that type; otherwise it is text. Header detection (§5.6) uses its own threshold, `header_type_threshold` (default 75%).

### 9.2 Behaviour

- A header click cycles **ascending → descending → original order**. A sort arrow in the header shows the state.
- Only one sort column. Sorting a new column replaces the previous sort.
- Header right-click: *Sort ascending*, *Sort descending*, *Original order*, and **Sort as ▸ Auto / Text / Number / Date**. The *Sort as* override lasts for the current file only.
- The right-click menu in the grid has *Restore original order*.
- **Comparators:**
  - Number and date columns compare on parsed keys. Cells that don't parse sort after the parsed ones, compared as text.
  - Text columns use `CompareStringEx` with `LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS`, i.e. case-insensitive natural order.
  - **Empty cells always sort last**, in both directions.
- The sort is stable (`std::stable_sort` over a `uint32_t` permutation). Keys are pre-extracted into a flat array first.
- It runs synchronously with a wait cursor. The file limit keeps that bounded.
- **Selection after a sort:** the current cell keeps its data row and moves with it. A rectangular selection collapses to the current cell.

---

## 10. Right-click menus

**Grid:**
- Copy · Copy as CSV · Copy with header · Select all
- Delimiter ▸ (Auto · `,` · `;` · Tab · `|`) · Encoding ▸ (Auto · UTF-8 · UTF-16 LE · UTF-16 BE · ANSI)
- ☐ First row is header · ☐ Show line breaks in cells · ☐ Auto-size columns · Last column ▸ (☐ Fully-fit last column · ☐ Wrap text in last column; exclusive, both may be off, §6.3.1) · ☐ Row numbers · ☐ Freeze first column · ☐ Status bar
- Restore original order · Reset zoom
- Settings…

**Header:**
- Sort ascending · Sort descending · Original order · Sort as ▸
- Fit column · Fit all columns
- Select column

**Persistence:**
- The ☐ toggles apply at once and are saved to the INI immediately, through `IniDocument`, which writes only changed keys and keeps comments.
- The delimiter and encoding submenus apply to the current file only; their defaults live in the INI.

---

## 11. Search

`ListSearchTextW(text, flags)`:
- Scans cells in **display order** (it respects the sort), row by row and left to right. The header row is included when present.
- It starts just after the current cell. With `lcs_findfirst`, it starts at the first cell, or the last cell when `lcs_backwards` is also set.
- Flags: `lcs_matchcase` (otherwise case-insensitive via `CompareStringEx`/`FindNLSStringEx` with `LINGUISTIC_IGNORECASE`), `lcs_wholewords` (word boundaries inside the cell text) and `lcs_backwards`.
- Matches the **full** cell text, ignoring the display cap.
- **On a hit:** the matching cell becomes the current cell and the selection, and it is scrolled into view (centered if it was off-screen). The matched substring inside the cell is tinted with `search_hit_background`.
- **No hit:** returns `LISTPLUGIN_ERROR`, and TC shows its own message.
- Highlighting all hits is out of scope for v1 (backlog).

---

## 12. Configuration: `tcl-csvvw.ini`

```ini
[General]
extensions=csv,tsv,tab        ; detect string source (TC caches it, see README)
max_file_size=100M            ; larger files get a notice instead of a table
max_cell_chars=32K            ; per-cell display/measure cap (search/copy use full text)
delimiter=auto                ; auto | , | ; | tab | pipe
encoding=auto                 ; auto | utf8 | utf16le | utf16be | ansi
header_mode=auto              ; auto | on | off
header_default=true           ; used when auto detection is inconclusive (menu toggle writes this)
type_threshold=90             ; % of non-empty cells that make a column number/date (sorting, alignment)
header_type_threshold=75      ; the same, for header detection only

font_face=Segoe UI
font_size=10
zoom=100                      ; 50..400, written on close when remember_zoom=true
remember_zoom=true

auto_size_columns=true
max_auto_width=400            ; px at 100% zoom
min_column_width=40
default_column_width=100      ; used when auto_size_columns=false

max_fit_width=4000            ; hard limit for fully-fit / divider double-click (px at 100%)
last_column=normal            ; normal | fit | wrap  (right-click > Last column)

multiline_cells=false
max_cell_lines=8              ; also limits wrapped lines in the last column
linebreak_fallback=¶          ; drawn for a cell's own line breaks when the font has no ↵
show_row_numbers=true
freeze_first_column=false
show_status_bar=true
zebra_rows=true
align_numbers_right=true

theme=auto                    ; auto | light | dark
debug_log=false

[ColorsLight]
; background=#FFFFFF  ... (empty = built-in; keys listed in §6.5)
[ColorsDark]
```

**INI encoding:** UTF-8 (with or without BOM), UTF-16 LE/BE with BOM and ANSI are all read. Writes keep the file's encoding; an ANSI file is rewritten as UTF-8, which is still detected correctly. This matters for non-ASCII values such as `linebreak_fallback=¶`. The INI format has no inline comments, so `;` and `#` inside values are safe.

**Settings window** (right-click → *Settings…*): the reused `ConfigDialog` with *OK / Cancel / Apply / Open INI file*. Pages are generated from `OptionSchema`:

1. **General:** extensions (with a read-only detect string and *Copy*), max file size, max cell chars, delimiter, encoding, header mode and default.
2. **View:** font face (dropdown of all installed fonts) and size, zoom and remember zoom, auto-size and widths (including `max_fit_width`), last column mode, multi-line and max lines, row numbers, freeze, status bar, zebra, number alignment.
3. **Colors:** theme, plus a list of color keys for light and dark with swatches, a `ChooseColor` picker and *Reset to built-in*.

Apply reloads the current file and keeps the scroll position and current cell when possible.

---

## 13. Robustness

- Never crash TC; never let exceptions cross the ABI.
- Every pass is linear-time.
- Pathological input is bounded by the caps:
  - `max_cell_chars` covers huge cells and long single lines.
  - A 16,384-column limit catches files that are not really CSV. Beyond it, the remaining fields are merged into the last column, with an InfoBar notice.
- A missing or invalid INI falls back to the defaults. A bad color is ignored.
- No leaks across `ListLoadNext`.
- File contents are never written anywhere except the clipboard, and only on explicit copy.

---

## 14. Performance targets (mid-range Win10 PC)

| Scenario | Target |
|---|---|
| 5,000 rows × 20 cols (~1 MB) | open < 50 ms |
| 100 MB, ~1M rows | open < 2 s (read + decode + index + stats) |
| Scroll / repaint, any file | < 16 ms per frame |
| Sort 1M rows | < 1.5 s (text), < 0.5 s (number) |
| Search full scan, 100 MB, no hit | < 1 s |
| Memory | ≈ file size (UTF-8 buffer) + 4 B/row + caches |

The plugin_smoke test logs the timings. CI reports them but does not gate on them.

---

## 15. Testing

- **Core unit tests** (doctest, Linux and MSVC):
  - RFC 4180 corpus: quotes, `""`, embedded delimiters and newlines, CRLF/LF/CR, lenient cases, unterminated quote.
  - Wrap layout (portable, with an injected width-measure callback): word and character breaks, the `max_cell_lines` cap, no soft wrap for cells with their own line breaks.
  - Ragged padding, BOMs and encodings, delimiter sniffing (including the `.tsv` tie-break), the header heuristic (definitive yes, no and inconclusive), type detection (number and date forms, 90% rule).
  - Comparators: empties last, stable order, the natural-text callback.
  - Search: flags, backwards, wrap stop.
  - TSV and CSV serialisers: quoting rules, single-cell raw copy.
  - Cell cap.
- **plugin_smoke** (Windows) loads the real DLL in a fake `TLister` host. It checks load and `ListLoadNext`, the detect string, `lc_copy` clipboard content, `lc_selectall`, search, sort through simulated header clicks, the oversized-file notice, encoding samples and timings.
- **CI:** `windows-msvc` (build, test, package artifact), `linux-core` (ASan/UBSan) and `mingw-cross` (compile check), as in tcl-syntaxhl.

---

## 16. Repository layout

```
CMakeLists.txt  SPECS.md  README.md
cmake/mingw-w64-x86_64.cmake
src/
  core/    Config, Settings, OptionSchema, IniDocument, TextDecoder, TextUtil
  csv/     Parser (row index, field split), Table (loader, sniffer, column stats, header detection),
           ValueParse, Ops (sort, search, clipboard), TextLayout (display transforms, word wrap)
  plugin/  exports.cpp, listplug.h, tcl-csvvw.def, WinUtil, DebugLog
  view/    ListerWindow, GridView, StatusBar, InfoBar
  ui/      ContextMenu, ConfigDialog, ConfigPages, DarkMode, resources.rc, version.h.in
dist/      tcl-csvvw.ini, pluginst.inf
tests/     test_*.cpp, samples/, third_party/doctest.h
tools/     plugin_smoke.cpp
.github/workflows/  ci.yml, release.yml
```

---

## 17. Milestones

0. **Scaffold:** extract the tcl-syntaxhl boilerplate (§2.2) and rename it. The plugin loads and shows an empty grid window. CI is green on all three jobs.
1. **Core parser:** loader, decoder to UTF-8, sniffer, row index, column stats, header heuristic, type detection, plus the full unit test corpus.
2. **Grid MVP:** rendering, scrolling, font, Ctrl+wheel zoom and remember-zoom, auto-size, gutter, status bar, dark and light palettes, percent reporting, key forwarding.
3. **Selection and clipboard:** mouse and keyboard selection, TSV, CSV and with-header copy, `lc_copy`/`lc_selectall`.
4. **Sort:** header click cycle, comparators, *Sort as* override, stable permutation.
5. **Search:** `ListSearchText` with all flags and the in-cell hit tint.
6. **Menus and modes:** delimiter and encoding overrides, header toggle, multi-line mode, last column mode (fit / wrap), variable row heights, freeze first column, tooltips, the oversized notice.
7. **Settings window:** General, View and Colors pages, with dark mode.
8. **Packaging:** README (including detect-string refresh), release zip, `pluginst.inf`, size check against the < 300 KB target.

---

## 18. Acceptance criteria (v1)

- [ ] The zip installs in TC x64. F3 on `.csv/.tsv/.tab` shows a table; other extensions are not claimed.
- [ ] Comma, semicolon, tab and pipe files are detected correctly. Quoted multi-line fields stay one row. Ragged rows are padded.
- [ ] UTF-8 (with and without BOM), UTF-16 LE/BE and ANSI files display correctly. The menu overrides re-parse the file.
- [ ] The header is auto-detected per §5.6. The menu checkbox toggles it and is remembered.
- [ ] A 5,000-row file opens instantly. A 100 MB file opens within target and scrolls smoothly. Larger files show the notice.
- [ ] Font face and size apply. Ctrl+wheel zooms, and the zoom is remembered when enabled.
- [ ] Auto-size fits columns up to the cap. Double-clicking a divider fits past the cap. Auto-size can be turned off.
- [ ] Cell, row, column, rectangle and all selection work with the mouse and keyboard. Ctrl+C pastes into Excel as cells.
- [ ] Header click cycles asc → desc → original. Numbers, dates and text sort correctly, with empties last.
- [ ] F7 / F3 / Shift+F3 find text with the case, whole-word and backwards options.
- [ ] Row numbers, freeze first column, status bar and multi-line toggles work and persist.
- [ ] The `↵` glyph shows for a cell's own line breaks in every mode.
- [ ] *Fully-fit last column* and *Wrap text in last column* are exclusive and persist. Wrap fills to the window edge, re-wraps on resize, caps at `max_cell_lines`, and never soft-wraps a cell that has its own line breaks.
- [ ] Dark mode follows TC. INI color overrides apply.
- [ ] Lister hotkeys still work. Quick View does not steal focus. No beeps.
- [ ] CI is green. The release zip is < 300 KB unpacked DLL size (target).

---

## 19. Implementation notes (v1 as built)

- **Portable core:** everything below the window layer is plain C++20 and unit-tested on Linux (ASan/UBSan) and Windows. Locale-aware parts are injected: `LCMapStringEx(LCMAP_SORTKEY | LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS)` sort keys and `FindNLSStringEx` case-insensitive search. An ASCII needle in an ASCII cell skips the NLS call. The portable fallbacks are ASCII case folding and natural digit order.
- **Sort keys** are built once per sort (binary keys, `memcmp` order), so the comparator never calls into the locale.
- **Text measuring** calls `GetTextExtentExPointW` with `lpnFit = NULL`. With a fit pointer and `nMaxExtent = 0`, Windows fills the extents only for the characters that "fit" (none), so all glyphs were drawn on one spot. Wine fills them all, which hid the bug. The smoke test now renders the grid through `WM_PRINTCLIENT` and checks that the text covers enough pixel columns.
- **Text drawing** passes the measured advances (`GetTextExtentExPointW`) to `ExtTextOutW`, so the search highlight, the `…` and the glyph runs line up exactly with the text.
- **Row cache:** the decoded UTF-16 text of up to 4096 rows. It is cleared when full.
- **Scroll bars** always show, disabled when not needed (`SIF_DISABLENOSCROLL`). This keeps the client width stable, so wrap mode cannot oscillate between widths.
- **Fixed-height rows** use row units on the vertical scroll bar. Variable-height rows (multi-line or wrap) use pixels.
- **Smoke test:** `tools/plugin_smoke` runs on `windows-latest` in CI, and locally under Wine + Xvfb. `CSVVW_SMOKE_SHOTS=<dir>` writes screenshots of each mode.
- **Debug log:** `debug_log=true` writes `tcl-csvvw.log` (load, parse time, window creation failures).

---

## 20. Open questions / assumptions

1. **The cell is the smallest selection unit.** Partial text selection inside a cell is out of scope for v1. If needed later: a read-only edit overlay on double-click.
2. **Oversized files** are claimed with a notice rather than returning `NULL`, so the reason is visible. An alternative is `oversize_action=decline`.
3. **`DD.MM.YYYY` is the only non-ISO date form.** US-style dates are treated as text.
4. **Manual column widths and per-file overrides** are not remembered between files.
