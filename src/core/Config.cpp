#include "Config.h"

#include "OptionSchema.h"
#include "TextUtil.h"

namespace csvvw {

const std::vector<ColorKey>& colorKeys() {
    static const std::vector<ColorKey> keys = {
        {"background", "Background", &Palette::background},
        {"text", "Text", &Palette::text},
        {"grid_lines", "Grid lines", &Palette::gridLines},
        {"header_background", "Header background", &Palette::headerBackground},
        {"header_text", "Header text", &Palette::headerText},
        {"alt_row_background", "Alternate row background", &Palette::altRowBackground},
        {"selection_background", "Selection background", &Palette::selectionBackground},
        {"selection_text", "Selection text", &Palette::selectionText},
        {"current_cell_border", "Current cell border", &Palette::currentCellBorder},
        {"search_hit_background", "Search hit background", &Palette::searchHitBackground},
        {"gutter_background", "Row number background", &Palette::gutterBackground},
        {"gutter_text", "Row number text", &Palette::gutterText},
        {"frozen_divider", "Frozen column divider", &Palette::frozenDivider},
        {"glyph", "Line break / ellipsis glyph", &Palette::glyph},
        {"info_background", "Notice bar background", &Palette::infoBackground},
        {"info_text", "Notice bar text", &Palette::infoText},
    };
    return keys;
}

Palette builtinPalette(bool dark) {
    if (!dark)
        return Palette{0xFFFFFF, 0x000000, 0xE1E1E1, 0xF0F0F0, 0x1A1A1A, 0xF6F8FB, 0xCCE4F7, 0x000000,
                       0x2A6FB0, 0xFFD54F, 0xF0F0F0, 0x707070, 0x9A9A9A, 0xA8A8A8, 0xFFF4CE, 0x000000};
    return Palette{0x1E1E1E, 0xD4D4D4, 0x3A3A3A, 0x2B2B2B, 0xE0E0E0, 0x242426, 0x264F78, 0xFFFFFF,
                   0x5C9DD8, 0x8A6D00, 0x2B2B2B, 0x8A8A8A, 0x707070, 0x6E6E6E, 0x4D4321, 0xF0F0F0};
}

Config Config::fromIni(const IniDocument& doc) {
    Config c;
    for (const auto& o : generalOptions())
        if (auto v = doc.get(kGeneral, o.key)) o.set(c, *v);  // invalid values keep the default
    for (const auto& k : colorKeys()) {
        if (auto v = doc.get(kColorsLight, k.key); v && !trim(*v).empty()) c.colorsLight[k.key] = std::string(trim(*v));
        if (auto v = doc.get(kColorsDark, k.key); v && !trim(*v).empty()) c.colorsDark[k.key] = std::string(trim(*v));
    }
    return c;
}

bool Config::handlesExtension(const std::filesystem::path& file) const {
    auto ext = pathToUtf8(file.extension());
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    for (auto& e : extensions)
        if (iequals(e, ext)) return true;
    return false;
}

std::string Config::buildDetectString(size_t maxlen) const {
    std::string s;
    for (auto& e : extensions) {
        std::string up = e;
        for (auto& ch : up)
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        std::string term = "EXT=\"" + up + "\"";
        std::string next = s.empty() ? term : s + " | " + term;
        if (maxlen && next.size() + 1 > maxlen) break;
        s = std::move(next);
    }
    return s;
}

Palette Config::palette(bool dark) const {
    Palette p = builtinPalette(dark);
    const auto& over = dark ? colorsDark : colorsLight;
    for (const auto& k : colorKeys()) {
        auto it = over.find(k.key);
        uint32_t v;
        if (it != over.end() && parseColor(it->second, v)) p.*(k.member) = v;
    }
    return p;
}

}  // namespace csvvw
