#include "OptionSchema.h"

#include <cstdlib>
#include <deque>

#include "TextUtil.h"

namespace csvvw {

bool parseBool(std::string_view s, bool& out) {
    s = trim(s);
    for (const char* t : {"true", "yes", "on", "1"})
        if (iequals(s, t)) { out = true; return true; }
    for (const char* f : {"false", "no", "off", "0"})
        if (iequals(s, f)) { out = false; return true; }
    return false;
}

namespace {

bool parseIntRange(std::string_view s, long long lo, long long hi, int& out) {
    s = trim(s);
    if (s.empty() || s.size() > 10) return false;
    bool neg = s[0] == '-';
    if (neg) s.remove_prefix(1);
    if (s.empty()) return false;
    long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (neg) v = -v;
    if (v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

template <class E>
struct EnumMap {
    const char* name;
    E value;
};

template <class E, size_t N>
bool parseEnum(std::string_view s, const EnumMap<E> (&m)[N], E& out) {
    s = trim(s);
    for (auto& e : m)
        if (iequals(s, e.name)) { out = e.value; return true; }
    return false;
}
template <class E, size_t N>
std::string enumName(const EnumMap<E> (&m)[N], E v) {
    for (auto& e : m)
        if (e.value == v) return e.name;
    return m[0].name;
}

const EnumMap<csv::HeaderMode> kHeaderModes[] = {
    {"auto", csv::HeaderMode::Auto}, {"on", csv::HeaderMode::On}, {"off", csv::HeaderMode::Off}};
const EnumMap<LastColumnMode> kLastColumn[] = {
    {"normal", LastColumnMode::Normal}, {"fit", LastColumnMode::Fit}, {"wrap", LastColumnMode::Wrap}};
const EnumMap<ThemeChoice> kThemes[] = {
    {"auto", ThemeChoice::Auto}, {"light", ThemeChoice::Light}, {"dark", ThemeChoice::Dark}};

std::string boolStr(bool b) { return b ? "true" : "false"; }

OptionDef boolOpt(const char* key, const char* label, bool Config::*m) {
    OptionDef d{key, OptionType::Bool, label, (Config{}.*m) ? "true" : "false"};
    d.get = [m](const Config& c) { return boolStr(c.*m); };
    d.set = [m](Config& c, std::string_view v) { return parseBool(v, c.*m); };
    return d;
}

OptionDef intOpt(const char* key, const char* label, int Config::*m, int lo, int hi) {
    static std::deque<std::string> defaults;  // keeps defaultValue strings alive (stable addresses)
    defaults.push_back(std::to_string(Config{}.*m));
    OptionDef d{key, OptionType::Int, label, nullptr, lo, hi};
    d.defaultValue = defaults.back().c_str();
    d.get = [m](const Config& c) { return std::to_string(c.*m); };
    d.set = [m, lo, hi](Config& c, std::string_view v) { return parseIntRange(v, lo, hi, c.*m); };
    return d;
}

OptionDef sizeOpt(const char* key, const char* label, uint64_t Config::*m, const char* def, long long lo,
                  long long hi) {
    OptionDef d{key, OptionType::Size, label, def, lo, hi};
    d.get = [m](const Config& c) { return formatSizeSpec(c.*m); };
    d.set = [m, lo, hi](Config& c, std::string_view v) {
        uint64_t x = 0;
        if (!parseSizeSpec(v, x) || x < static_cast<uint64_t>(lo) || x > static_cast<uint64_t>(hi)) return false;
        c.*m = x;
        return true;
    };
    return d;
}

template <class E, size_t N>
OptionDef enumOpt(const char* key, const char* label, E Config::*m, const EnumMap<E> (&map)[N]) {
    OptionDef d{key, OptionType::Enum, label, map[0].name};
    for (auto& e : map) d.choices.push_back(e.name);
    d.defaultValue = nullptr;
    for (auto& e : map)
        if (e.value == Config{}.*m) d.defaultValue = e.name;
    d.get = [m, &map](const Config& c) { return enumName(map, c.*m); };
    d.set = [m, &map](Config& c, std::string_view v) { return parseEnum(v, map, c.*m); };
    return d;
}

std::vector<OptionDef> buildOptions() {
    std::vector<OptionDef> o;
    {
        OptionDef d{"extensions", OptionType::List, "Extensions", "csv,tsv,tab"};
        d.get = [](const Config& c) {
            std::string s;
            for (auto& e : c.extensions) s += (s.empty() ? "" : ",") + e;
            return s;
        };
        d.set = [](Config& c, std::string_view v) {
            auto list = splitList(v, ',');
            if (list.empty()) return false;
            for (auto& e : list) {
                if (!e.empty() && e[0] == '.') e.erase(0, 1);
                e = toLowerAscii(e);
                if (e.empty() || e.find_first_of(" \"|*?") != std::string::npos) return false;
            }
            c.extensions = std::move(list);
            return true;
        };
        o.push_back(std::move(d));
    }
    o.push_back(sizeOpt("max_file_size", "Maximum file size", &Config::maxFileSize, "100M", 1024, 2147483647LL));
    o.push_back(sizeOpt("max_cell_chars", "Maximum characters shown per cell", &Config::maxCellChars, "32K", 256,
                        1024 * 1024));
    {
        OptionDef d{"delimiter", OptionType::Enum, "Delimiter", "auto"};
        d.choices = {"auto", ",", ";", "tab", "pipe"};
        d.get = [](const Config& c) { return std::string(csv::delimiterKey(c.delimiter)); };
        d.set = [](Config& c, std::string_view v) { return csv::parseDelimiterKey(v, c.delimiter); };
        o.push_back(std::move(d));
    }
    {
        OptionDef d{"encoding", OptionType::Enum, "Encoding", "auto"};
        d.choices = {"auto", "utf8", "utf16le", "utf16be", "ansi"};
        d.get = [](const Config& c) { return std::string(encodingChoiceKey(c.encoding)); };
        d.set = [](Config& c, std::string_view v) { return parseEncodingChoice(trim(v), c.encoding); };
        o.push_back(std::move(d));
    }
    o.push_back(enumOpt("header_mode", "First row is header", &Config::headerMode, kHeaderModes));
    o.push_back(boolOpt("header_default", "Header when detection is inconclusive", &Config::headerDefault));
    {
        OptionDef d{"font_face", OptionType::Font, "Font", "Segoe UI"};
        d.get = [](const Config& c) { return c.fontFace; };
        d.set = [](Config& c, std::string_view v) {
            v = trim(v);
            if (v.empty() || v.size() > 31) return false;
            c.fontFace = std::string(v);
            return true;
        };
        o.push_back(std::move(d));
    }
    o.push_back(intOpt("font_size", "Font size", &Config::fontSize, 4, 72));
    o.push_back(intOpt("zoom", "Zoom (%)", &Config::zoom, 50, 400));
    o.push_back(boolOpt("remember_zoom", "Remember zoom", &Config::rememberZoom));
    o.push_back(boolOpt("auto_size_columns", "Auto-size columns", &Config::autoSizeColumns));
    o.push_back(intOpt("max_auto_width", "Auto-size width limit", &Config::maxAutoWidth, 40, 4000));
    o.push_back(intOpt("min_column_width", "Minimum column width", &Config::minColumnWidth, 10, 400));
    o.push_back(intOpt("default_column_width", "Default column width", &Config::defaultColumnWidth, 20, 2000));
    o.push_back(intOpt("max_fit_width", "Fully-fit width limit", &Config::maxFitWidth, 100, 32000));
    o.push_back(enumOpt("last_column", "Last column", &Config::lastColumn, kLastColumn));
    o.push_back(boolOpt("multiline_cells", "Show line breaks in cells", &Config::multilineCells));
    o.push_back(intOpt("max_cell_lines", "Maximum lines per cell", &Config::maxCellLines, 1, 100));
    o.push_back(boolOpt("show_row_numbers", "Row numbers", &Config::showRowNumbers));
    o.push_back(boolOpt("freeze_first_column", "Freeze first column", &Config::freezeFirstColumn));
    o.push_back(boolOpt("show_status_bar", "Status bar", &Config::showStatusBar));
    o.push_back(boolOpt("zebra_rows", "Zebra rows", &Config::zebraRows));
    o.push_back(boolOpt("align_numbers_right", "Right-align numbers", &Config::alignNumbersRight));
    o.push_back(enumOpt("theme", "Theme", &Config::theme, kThemes));
    o.push_back(boolOpt("debug_log", "Debug log", &Config::debugLog));
    return o;
}

}  // namespace

std::string OptionDef::normalize(std::string_view value) const {
    Config c;
    if (!set(c, value)) return {};
    return get(c);
}

bool OptionDef::sameValue(std::string_view a, std::string_view b) const {
    auto na = normalize(a), nb = normalize(b);
    if (na.empty() || nb.empty()) return trim(a) == trim(b);
    return na == nb;
}

const std::vector<OptionDef>& generalOptions() {
    static const std::vector<OptionDef> opts = buildOptions();
    return opts;
}

const OptionDef* findGeneralOption(std::string_view key) {
    for (auto& o : generalOptions())
        if (iequals(o.key, key)) return &o;
    return nullptr;
}

std::string getGeneralValue(const Config& cfg, std::string_view key) {
    auto* o = findGeneralOption(key);
    return o ? o->get(cfg) : std::string();
}

bool setGeneralValue(Config& cfg, std::string_view key, std::string_view value) {
    auto* o = findGeneralOption(key);
    if (!o) return false;
    Config probe = cfg;
    if (!o->set(probe, value)) return false;
    cfg = std::move(probe);
    return true;
}

}  // namespace csvvw
