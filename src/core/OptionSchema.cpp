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

const std::vector<EnumMap<csv::HeaderMode>> kHeaderModes = {
    {"auto", csv::HeaderMode::Auto}, {"on", csv::HeaderMode::On}, {"off", csv::HeaderMode::Off}};
const std::vector<EnumMap<LastColumnMode>> kLastColumn = {
    {"normal", LastColumnMode::Normal}, {"fit", LastColumnMode::Fit}, {"wrap", LastColumnMode::Wrap}};
const std::vector<EnumMap<ThemeChoice>> kThemes = {
    {"auto", ThemeChoice::Auto}, {"light", ThemeChoice::Light}, {"dark", ThemeChoice::Dark}};

std::string boolStr(bool b) { return b ? "true" : "false"; }

// Accessors are plain functions returning a reference to the Config field.
// (Capturing pointers-to-member in std::function lambdas crashed MSVC 14.51's
// code generator, so the helpers avoid them.)
Config& defaultConfig() {
    static Config c;
    return c;
}

std::deque<std::string>& stringPool() {  // stable storage for generated default strings
    static std::deque<std::string> pool;
    return pool;
}

using BoolRef = bool& (*)(Config&);
using IntRef = int& (*)(Config&);
using SizeRef = uint64_t& (*)(Config&);

OptionDef boolOpt(const char* key, const char* label, BoolRef ref) {
    OptionDef d{key, OptionType::Bool, label, ref(defaultConfig()) ? "true" : "false"};
    d.get = [ref](const Config& c) { return boolStr(ref(const_cast<Config&>(c))); };
    d.set = [ref](Config& c, std::string_view v) { return parseBool(v, ref(c)); };
    return d;
}

OptionDef intOpt(const char* key, const char* label, IntRef ref, int lo, int hi) {
    stringPool().push_back(std::to_string(ref(defaultConfig())));
    OptionDef d{key, OptionType::Int, label, stringPool().back().c_str(), lo, hi};
    d.get = [ref](const Config& c) { return std::to_string(ref(const_cast<Config&>(c))); };
    d.set = [ref, lo, hi](Config& c, std::string_view v) { return parseIntRange(v, lo, hi, ref(c)); };
    return d;
}

OptionDef sizeOpt(const char* key, const char* label, SizeRef ref, const char* def, long long lo, long long hi) {
    OptionDef d{key, OptionType::Size, label, def, lo, hi};
    d.get = [ref](const Config& c) { return formatSizeSpec(ref(const_cast<Config&>(c))); };
    d.set = [ref, lo, hi](Config& c, std::string_view v) {
        uint64_t x = 0;
        if (!parseSizeSpec(v, x) || x < static_cast<uint64_t>(lo) || x > static_cast<uint64_t>(hi)) return false;
        ref(c) = x;
        return true;
    };
    return d;
}

template <class E>
OptionDef enumOpt(const char* key, const char* label, E& (*ref)(Config&), const std::vector<EnumMap<E>>* map) {
    OptionDef d{key, OptionType::Enum, label, (*map)[0].name};
    for (const auto& e : *map) {
        d.choices.push_back(e.name);
        if (e.value == ref(defaultConfig())) d.defaultValue = e.name;
    }
    d.get = [ref, map](const Config& c) {
        E v = ref(const_cast<Config&>(c));
        for (const auto& e : *map)
            if (e.value == v) return std::string(e.name);
        return std::string((*map)[0].name);
    };
    d.set = [ref, map](Config& c, std::string_view v) {
        v = trim(v);
        for (const auto& e : *map)
            if (iequals(v, e.name)) {
                ref(c) = e.value;
                return true;
            }
        return false;
    };
    return d;
}

#define CSVVW_REF(type, field) [](Config& c) -> type& { return c.field; }

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
    o.push_back(sizeOpt("max_file_size", "Maximum file size", CSVVW_REF(uint64_t, maxFileSize), "100M", 1024, 2147483647LL));
    o.push_back(sizeOpt("max_cell_chars", "Maximum characters shown per cell", CSVVW_REF(uint64_t, maxCellChars), "32K", 256,
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
    o.push_back(enumOpt<csv::HeaderMode>("header_mode", "First row is header", CSVVW_REF(csv::HeaderMode, headerMode), &kHeaderModes));
    o.push_back(boolOpt("header_default", "Header when detection is inconclusive", CSVVW_REF(bool, headerDefault)));
    o.push_back(intOpt("type_threshold", "Typed column threshold (%)", CSVVW_REF(int, typeThreshold), 50, 100));
    o.push_back(intOpt("header_type_threshold", "Header detection threshold (%)", CSVVW_REF(int, headerTypeThreshold),
                       50, 100));
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
    o.push_back(intOpt("font_size", "Font size", CSVVW_REF(int, fontSize), 4, 72));
    o.push_back(intOpt("zoom", "Zoom (%)", CSVVW_REF(int, zoom), 50, 400));
    o.push_back(boolOpt("remember_zoom", "Remember zoom", CSVVW_REF(bool, rememberZoom)));
    {
        // off | largest | typical; true/false accepted (older INI files).
        OptionDef d{"auto_size_columns", OptionType::Enum, "Auto-size columns", "largest"};
        d.choices = {"off", "largest", "typical"};
        d.get = [](const Config& c) {
            return std::string(c.autoSize == AutoSizeMode::Off       ? "off"
                               : c.autoSize == AutoSizeMode::Typical ? "typical"
                                                                     : "largest");
        };
        d.set = [](Config& c, std::string_view v) {
            v = trim(v);
            bool b;
            if (iequals(v, "largest")) c.autoSize = AutoSizeMode::Largest;
            else if (iequals(v, "typical")) c.autoSize = AutoSizeMode::Typical;
            else if (iequals(v, "off")) c.autoSize = AutoSizeMode::Off;
            else if (parseBool(v, b)) c.autoSize = b ? AutoSizeMode::Largest : AutoSizeMode::Off;
            else return false;
            return true;
        };
        o.push_back(std::move(d));
    }
    o.push_back(intOpt("auto_size_percentile", "Typical width percentile", CSVVW_REF(int, autoSizePercentile), 10, 100));
    o.push_back(intOpt("max_auto_width", "Auto-size width limit", CSVVW_REF(int, maxAutoWidth), 40, 4000));
    o.push_back(intOpt("min_column_width", "Minimum column width", CSVVW_REF(int, minColumnWidth), 10, 400));
    o.push_back(intOpt("default_column_width", "Default column width", CSVVW_REF(int, defaultColumnWidth), 20, 2000));
    o.push_back(intOpt("max_fit_width", "Fully-fit width limit", CSVVW_REF(int, maxFitWidth), 100, 32000));
    o.push_back(enumOpt<LastColumnMode>("last_column", "Last column", CSVVW_REF(LastColumnMode, lastColumn), &kLastColumn));
    o.push_back(boolOpt("multiline_cells", "Show line breaks in cells", CSVVW_REF(bool, multilineCells)));
    o.push_back(intOpt("max_cell_lines", "Maximum lines per cell", CSVVW_REF(int, maxCellLines), 1, 100));
    {
        OptionDef d{"linebreak_fallback", OptionType::Char, "Line break fallback glyph", "\xC2\xB6"};
        d.get = [](const Config& c) { return utf16ToUtf8(c.linebreakFallback); };
        d.set = [](Config& c, std::string_view v) {
            auto u = utf8ToUtf16(trim(v));
            if (u.size() != 1 || u[0] < 0x20 || (u[0] >= 0xD800 && u[0] <= 0xDFFF) || u[0] == 0xFFFD) return false;
            c.linebreakFallback = u;
            return true;
        };
        o.push_back(std::move(d));
    }
    o.push_back(boolOpt("show_row_numbers", "Row numbers", CSVVW_REF(bool, showRowNumbers)));
    o.push_back(boolOpt("freeze_first_column", "Freeze first column", CSVVW_REF(bool, freezeFirstColumn)));
    o.push_back(boolOpt("show_status_bar", "Status bar", CSVVW_REF(bool, showStatusBar)));
    o.push_back(boolOpt("zebra_rows", "Zebra rows", CSVVW_REF(bool, zebraRows)));
    o.push_back(boolOpt("align_numbers_right", "Right-align numbers", CSVVW_REF(bool, alignNumbersRight)));
    o.push_back(enumOpt<ThemeChoice>("theme", "Theme", CSVVW_REF(ThemeChoice, theme), &kThemes));
    o.push_back(boolOpt("debug_log", "Debug log", CSVVW_REF(bool, debugLog)));
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
