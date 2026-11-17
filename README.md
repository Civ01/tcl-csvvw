# tcl-csvvw

A fast, small CSV/TSV table viewer plugin for Total Commander's Lister (F3 / Ctrl+Q), 64-bit.

- Renders CSV as a virtual table: only the visible cells are drawn, so 5,000 rows open instantly and
  files up to 100 MB (configurable) stay smooth.
- Own RFC 4180 parser (no third-party libraries): quoted fields, `""` escapes, line breaks inside
  cells, CRLF / LF / CR, ragged rows (padded), lenient handling of stray quotes.
- Auto-detects the delimiter (`,` `;` Tab `|`), encoding (UTF-8 with/without BOM, UTF-16 LE/BE, ANSI)
  and whether the first row is a header. All three can be overridden from the right-click menu.
- Auto-sized columns (with a width cap), Ctrl+wheel zoom (optionally remembered), custom font.
- Spreadsheet-style selection (cell, row, column, rectangle) and copy as TSV (pastes into Excel as
  cells), as CSV, or with the header row.
- Click a column header to sort: ascending → descending → original order. Numbers, dates and text
  (natural, case-insensitive order) are detected per column; empty cells always sort last.
- Find with Total Commander's own search (F7, F3 / Shift+F3): match case, whole words, backwards.
- Row numbers, frozen first column, status bar, line breaks shown as `↵` or as real multi-line rows,
  last column fully-fit or wrapped, light/dark colors following Total Commander's dark mode.

## Installation

Open the release zip in Total Commander; it offers to install the plugin (`pluginst.inf`).
Manual installation: extract to a folder, then *Configuration → Options → Plugins → Lister plugins →
Add* and pick `tcl-csvvw.wlx64`. `tcl-csvvw.ini` must stay next to the plugin.

## Usage

| Action | Mouse / keys |
|---|---|
| Select cells | Click, drag, Shift+click; arrows, Shift+arrows, Home/End, Ctrl+Home/End, PgUp/PgDn |
| Select rows | Click / drag in the row-number column |
| Select columns | Ctrl+click a header, Ctrl+Shift+click or drag across headers |
| Select all | Ctrl+A, or click the top-left corner |
| Copy | Ctrl+C (TSV), Ctrl+Shift+C (CSV); right-click → *Copy with header* |
| Sort | Click a header (asc → desc → original); right-click a header → *Sort as* text / number / date |
| Column width | Drag a header divider; double-click it to fit (ignores the auto-size limit) |
| Zoom | Ctrl+wheel, Ctrl+`+` / Ctrl+`-`, Ctrl+0 to reset |
| Find | F7 (Total Commander's dialog), F3 / Shift+F3 next / previous |

All other keys go to Lister (Esc, N/P, 1–7, …).

Right-click the table for: delimiter and encoding overrides (current file), *First row is header*,
*Show line breaks in cells*, *Auto-size columns → By largest / typical value*, *Last column → Fully-fit / Wrap text*,
*Row numbers*, *Freeze first column*, *Status bar*, *Restore original order*, *Reset zoom* and
*Settings…*. Toggles are saved to the INI immediately.

## Configuration

Everything is in `tcl-csvvw.ini` (fully commented; UTF-8, UTF-16 with BOM or ANSI, and the encoding is kept on save) or in the settings window (right-click →
*Settings…*). Highlights:

| Key | Default | Meaning |
|---|---|---|
| `extensions` | `csv,tsv,tab` | Files handled by the plugin |
| `max_file_size` | `100M` | Larger files show a notice instead of a table |
| `max_cell_chars` | `32K` | Characters drawn per cell (search and copy use the full text) |
| `delimiter` / `encoding` | `auto` | Defaults; the right-click menu overrides them per file |
| `header_mode` / `header_default` | `auto` / `true` | Header detection; the default applies when detection is inconclusive |
| `type_threshold` / `header_type_threshold` | `90` / `75` | % of a column's cells that must be numbers/dates to sort as such / to detect a header |
| `font_face` / `font_size` | `Segoe UI` / `10` | Table font |
| `zoom` / `remember_zoom` | `100` / `true` | Start zoom; with remember_zoom the last zoom is saved |
| `auto_size_columns`, `max_auto_width` | `largest`, `400` | `largest` fits the widest value, `typical` fits the value length at `auto_size_percentile` (90) so rare long values don't widen the column, `off`; up to the limit (px at 100%) |
| `max_fit_width` | `4000` | Limit for fully-fit last column / divider double-click |
| `last_column` | `normal` | `fit` = as wide as its widest cell, `wrap` = wrap long text, filling the window |
| `multiline_cells`, `max_cell_lines` | `false`, `8` | Show a cell's own line breaks as separate lines |
| `linebreak_fallback` | `¶` | Drawn for line breaks when the font has no `↵` glyph |
| `theme` | `auto` | `auto` follows Total Commander's dark mode; colors can be overridden in `[ColorsLight]` / `[ColorsDark]` |

### Changing file extensions

Total Commander caches the plugin's detect string when the plugin is installed. After changing
`extensions`, copy the new detect string from *Settings… → General* and paste it into
*Configuration → Options → Plugins → Lister plugins → Configure* (or re-add the plugin).

## Building

Requirements: CMake ≥ 3.20 and Visual Studio 2022 (MSVC v143), or MinGW-w64 for a compile check.

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Release --target package_zip
```

The core (parser, table model, sorting, search, layout, configuration) is portable C++20 and its
unit tests also run on Linux:

```
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake --build build-linux && ctest --test-dir build-linux
```

`tools/plugin_smoke` drives the real DLL in a fake Lister window (load, copy, search, sorting, every
display mode, oversized files, timing). Set `CSVVW_SMOKE_SHOTS=<dir>` to save screenshots.

Releases: push a tag `vX.Y.Z` (or `vX.Y.Z-label` for a pre-release); the release workflow builds,
tests and publishes `tcl-csvvw-<version>-x64.zip`.

See [SPECS.md](SPECS.md) for the full specification.
