#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "core/Config.h"
#include "core/OptionSchema.h"
#include "core/Settings.h"
#include "core/TextUtil.h"

using namespace csvvw;
namespace fs = std::filesystem;

TEST_CASE("size specs") {
    uint64_t v;
    CHECK(parseSizeSpec("100M", v));
    CHECK(v == 100ull << 20);
    CHECK(parseSizeSpec("32k", v));
    CHECK(v == 32768);
    CHECK(parseSizeSpec("2 GB", v));
    CHECK(v == 2ull << 30);
    CHECK_FALSE(parseSizeSpec("abc", v));
    CHECK(formatSizeSpec(104857600) == "100M");
    CHECK(formatSizeSpec(1000) == "1000");
    CHECK(formatSize(250ull << 20) == "250 MB");
}

TEST_CASE("defaults match the schema") {
    Config c;
    for (auto& o : generalOptions()) {
        INFO(o.key);
        CHECK(o.sameValue(o.defaultValue, o.get(c)));
    }
}

TEST_CASE("config from ini") {
    auto doc = IniDocument::parse(std::string(
        "[General]\nextensions=csv, .TXT\nmax_file_size=10M\ndelimiter=;\nencoding=utf16le\nheader_mode=off\n"
        "font_size=99\nlast_column=wrap\nzoom=150\nfreeze_first_column=yes\n"
        "[ColorsDark]\nbackground=#102030\ntext=bogus\n"));
    auto c = Config::fromIni(doc);
    CHECK(c.extensions == std::vector<std::string>{"csv", "txt"});
    CHECK(c.maxFileSize == 10ull << 20);
    CHECK(c.delimiter == ';');
    CHECK(c.encoding == EncodingChoice::Utf16LE);
    CHECK(c.headerMode == csv::HeaderMode::Off);
    CHECK(c.fontSize == 10);  // out of range: default kept
    CHECK(c.lastColumn == LastColumnMode::Wrap);
    CHECK(c.zoom == 150);
    CHECK(c.freezeFirstColumn);
    auto p = c.palette(true);
    CHECK(p.background == 0x102030);
    CHECK(p.text == builtinPalette(true).text);  // invalid override ignored
    CHECK(c.handlesExtension("x/y/data.TXT"));
    CHECK_FALSE(c.handlesExtension("data.json"));
}

TEST_CASE("detect string") {
    Config c;
    CHECK(c.buildDetectString(1024) == "EXT=\"CSV\" | EXT=\"TSV\" | EXT=\"TAB\"");
    CHECK(c.buildDetectString(20) == "EXT=\"CSV\"");
}

TEST_CASE("settings save only changed values and keep comments") {
    auto dir = fs::temp_directory_path() / "csvvw_test_settings";
    fs::create_directories(dir);
    auto ini = dir / "t.ini";
    {
        std::ofstream f(ini, std::ios::binary);
        f << "; my comment\r\n[General]\r\nmax_file_size=104857600\r\nzoom=100\r\n";
    }
    Settings s;
    REQUIRE(s.load(ini));
    s.config().zoom = 120;
    s.config().colorsLight["grid_lines"] = "#abcdef";
    REQUIRE(s.save());
    std::string out;
    REQUIRE(readFileBytes(ini, out));
    CHECK(out.find("; my comment") != std::string::npos);
    CHECK(out.find("max_file_size=104857600") != std::string::npos);  // equal value not rewritten
    CHECK(out.find("zoom=120") != std::string::npos);
    CHECK(out.find("[ColorsLight]") != std::string::npos);
    CHECK(out.find("grid_lines=#ABCDEF") != std::string::npos);
    CHECK(out.find("font_face") == std::string::npos);  // defaults are not added

    REQUIRE(Settings::saveGeneralValue(ini, "header_default", "false"));
    REQUIRE(Settings::saveGeneralValue(ini, "zoom", "120"));  // unchanged: no-op
    REQUIRE(readFileBytes(ini, out));
    CHECK(out.find("header_default=false") != std::string::npos);
    CHECK_FALSE(Settings::saveGeneralValue(ini, "no_such_key", "1"));
    fs::remove_all(dir);
}

TEST_CASE("dist ini parses with defaults") {
    std::string bytes;
    REQUIRE(readFileBytes(fs::path(CSVVW_SOURCE_DIR) / "dist" / "tcl-csvvw.ini", bytes));
    auto doc = IniDocument::parse(bytes);
    auto c = Config::fromIni(doc);
    Config d;
    for (auto& o : generalOptions()) {
        INFO(o.key);
        CHECK(o.get(c) == o.get(d));
        CHECK(doc.get(kGeneral, o.key).has_value());  // every option is documented
    }
}

TEST_CASE("ini encodings: non-ASCII values and round trips") {
    auto dir = fs::temp_directory_path() / "csvvw_test_ini_enc";
    fs::create_directories(dir);
    auto ini = dir / "e.ini";
    const std::string body = "[General]\r\nlinebreak_fallback=\xE2\x8F\x8E\r\n";  // U+23CE
    auto writeBytes = [&](const std::string& b) {
        std::ofstream f(ini, std::ios::binary | std::ios::trunc);
        f << b;
    };
    auto utf16 = [&](bool be) {
        std::string b = be ? std::string("\xFE\xFF", 2) : std::string("\xFF\xFE", 2);
        for (char16_t c : utf8ToUtf16(body)) {
            char hi = static_cast<char>(c >> 8), lo = static_cast<char>(c & 0xFF);
            b.push_back(be ? hi : lo);
            b.push_back(be ? lo : hi);
        }
        return b;
    };
    struct Case {
        const char* name;
        std::string bytes;
        Encoding enc;
    };
    for (const Case& c : {Case{"utf8", body, Encoding::Utf8}, Case{"utf8bom", "\xEF\xBB\xBF" + body, Encoding::Utf8Bom},
                          Case{"utf16le", utf16(false), Encoding::Utf16LE},
                          Case{"utf16be", utf16(true), Encoding::Utf16BE}}) {
        INFO(c.name);
        writeBytes(c.bytes);
        Settings s;
        REQUIRE(s.load(ini));
        CHECK(s.document().encoding() == c.enc);
        CHECK(s.config().linebreakFallback == u"\u23CE");
        REQUIRE(Settings::saveGeneralValue(ini, "zoom", "150"));
        Settings again;
        REQUIRE(again.load(ini));
        CHECK(again.document().encoding() == c.enc);  // encoding kept
        CHECK(again.config().zoom == 150);
        CHECK(again.config().linebreakFallback == u"\u23CE");
    }
    // ANSI (cp1252 pilcrow 0xB6) reads through the converter, is rewritten as UTF-8
    writeBytes("[General]\r\nlinebreak_fallback=\xB6\r\n");
    Settings a;
    REQUIRE(a.load(ini));
    CHECK(a.config().linebreakFallback == u"\u00B6");  // Latin-1 fallback converter
    REQUIRE(Settings::saveGeneralValue(ini, "zoom", "120"));
    Settings b;
    REQUIRE(b.load(ini));
    CHECK(b.document().encoding() == Encoding::Utf8);
    CHECK(b.config().linebreakFallback == u"\u00B6");
    // invalid fallback values keep the default
    Config c;
    CHECK_FALSE(setGeneralValue(c, "linebreak_fallback", "ab"));
    CHECK_FALSE(setGeneralValue(c, "linebreak_fallback", ""));
    CHECK(setGeneralValue(c, "linebreak_fallback", "|"));
    fs::remove_all(dir);
}

TEST_CASE("thresholds are options") {
    Config c;
    CHECK(c.typeThreshold == 90);
    CHECK(c.headerTypeThreshold == 75);
    CHECK(setGeneralValue(c, "header_type_threshold", "60"));
    CHECK(c.headerTypeThreshold == 60);
    CHECK_FALSE(setGeneralValue(c, "type_threshold", "40"));
}

TEST_CASE("auto_size_columns: modes and legacy booleans") {
    Config c;
    CHECK(c.autoSize == AutoSizeMode::Largest);
    CHECK(setGeneralValue(c, "auto_size_columns", "typical"));
    CHECK(c.autoSize == AutoSizeMode::Typical);
    CHECK(setGeneralValue(c, "auto_size_columns", "false"));
    CHECK(c.autoSize == AutoSizeMode::Off);
    CHECK(setGeneralValue(c, "auto_size_columns", "true"));
    CHECK(c.autoSize == AutoSizeMode::Largest);
    CHECK(getGeneralValue(c, "auto_size_columns") == "largest");
    CHECK_FALSE(setGeneralValue(c, "auto_size_columns", "maybe"));
    CHECK(setGeneralValue(c, "auto_size_percentile", "50"));
    CHECK_FALSE(setGeneralValue(c, "auto_size_percentile", "5"));
}
