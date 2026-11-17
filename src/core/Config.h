// Plugin configuration (tcl-csvvw.ini, SPECS §12).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "IniDocument.h"
#include "TextDecoder.h"
#include "csv/Table.h"

namespace csvvw {

enum class ThemeChoice { Auto, Light, Dark };
enum class LastColumnMode { Normal, Fit, Wrap };
enum class AutoSizeMode { Off, Largest, Typical };

// View colors, 0xRRGGBB.
struct Palette {
    uint32_t background, text, gridLines, headerBackground, headerText, altRowBackground, selectionBackground,
        selectionText, currentCellBorder, searchHitBackground, gutterBackground, gutterText, frozenDivider, glyph,
        infoBackground, infoText;
};

struct ColorKey {
    const char* key;            // INI key in [ColorsLight] / [ColorsDark]
    const char* label;          // UI label
    uint32_t Palette::*member;  // field
};
const std::vector<ColorKey>& colorKeys();
Palette builtinPalette(bool dark);

struct Config {
    // [General]
    std::vector<std::string> extensions{"csv", "tsv", "tab"};
    uint64_t maxFileSize = 100ull * 1024 * 1024;
    uint64_t maxCellChars = 32 * 1024;
    char delimiter = 0;  // 0 = auto
    EncodingChoice encoding = EncodingChoice::Auto;
    csv::HeaderMode headerMode = csv::HeaderMode::Auto;
    bool headerDefault = true;
    int typeThreshold = 90;        // % numeric/date cells for a typed column (sorting, alignment)
    int headerTypeThreshold = 75;  // same, for header detection

    std::string fontFace = "Segoe UI";
    int fontSize = 10;
    int zoom = 100;
    bool rememberZoom = true;

    AutoSizeMode autoSize = AutoSizeMode::Largest;
    int autoSizePercentile = 90;  // "typical" = this percentile of the cell lengths
    int maxAutoWidth = 400;
    int minColumnWidth = 40;
    int defaultColumnWidth = 100;
    int maxFitWidth = 4000;
    LastColumnMode lastColumn = LastColumnMode::Normal;

    bool multilineCells = false;
    int maxCellLines = 8;
    std::u16string linebreakFallback = u"\u00B6";  // drawn instead of the line-break glyph when the font lacks it
    bool showRowNumbers = true;
    bool freezeFirstColumn = false;
    bool showStatusBar = true;
    bool zebraRows = true;
    bool alignNumbersRight = true;

    ThemeChoice theme = ThemeChoice::Auto;
    bool debugLog = false;

    // Color overrides ("#RRGGBB"); missing / empty = built-in.
    std::map<std::string, std::string> colorsLight, colorsDark;

    static Config defaults() { return Config{}; }
    static Config fromIni(const IniDocument& doc);

    bool handlesExtension(const std::filesystem::path& file) const;
    // EXT="CSV" | EXT="TSV" ..., truncated at whole terms to fit maxlen-1 chars.
    std::string buildDetectString(size_t maxlen) const;
    bool useDark(bool tcDark) const {
        return theme == ThemeChoice::Dark || (theme == ThemeChoice::Auto && tcDark);
    }
    Palette palette(bool dark) const;
};

constexpr const char* kGeneral = "General";
constexpr const char* kColorsLight = "ColorsLight";
constexpr const char* kColorsDark = "ColorsDark";

}  // namespace csvvw
