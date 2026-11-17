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
