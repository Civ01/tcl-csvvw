#include "TestUtil.h"

#include <algorithm>

using namespace csvvw;
using namespace csvvw::csv;
using csvvw::test::load;

TEST_CASE("sniff delimiter") {
    CHECK(sniffDelimiter("a,b,c\n1,2,3\n", false) == ',');
    CHECK(sniffDelimiter("a;b;c\n1,5;2;3\n", false) == ';');
    CHECK(sniffDelimiter("a\tb\tc\n1\t2\t3\n", false) == '\t');
    CHECK(sniffDelimiter("a|b|c\n1|2|3\n", false) == '|');
    CHECK(sniffDelimiter("\"x;y\",b\n\"1;2\",3\n", false) == ',');  // quote-aware
    CHECK(sniffDelimiter("single\ncolumn\n", false) == ',');
    // equally consistent: Tab wins for .tsv, comma otherwise
    CHECK(sniffDelimiter("a,b\tc\n1,2\t3\n", true) == '\t');
    CHECK(sniffDelimiter("a,b\tc\n1,2\t3\n", false) == ',');
}

TEST_CASE("load: ragged rows are padded to the widest row") {
    auto t = load("a,b\n1,2,3,4\n5\n");
    CHECK(t->columns == 4);
    CHECK(t->fileRows() == 3);
    CHECK(t->cell(2, 0) == "5");
    CHECK(t->cell(2, 3).empty());
    CHECK(t->cell(1, 3) == "4");
}

TEST_CASE("load: forced delimiter") {
    LoadOptions o;
    o.delimiter = ';';
    auto t = load("a,b;c\n", o);
    CHECK_FALSE(t->delimiterDetected);
    CHECK(t->columns == 2);
    CHECK(t->cell(0, 0) == "a,b");
}

TEST_CASE("header detection") {
    SUBCASE("text header over numeric column -> yes") {
        auto t = load("name,age\nbob,12\nann,7\n");
        CHECK(t->headerDetect == HeaderDetect::Yes);
        CHECK(t->header);
        CHECK(t->dataRows() == 2);
        CHECK(t->caption(1) == "age");
    }
    SUBCASE("numeric first row in numeric column -> no") {
        auto t = load("1,2\n3,4\n");
        CHECK(t->headerDetect == HeaderDetect::No);
        CHECK_FALSE(t->header);
        CHECK(t->caption(0) == "A");
    }
    SUBCASE("all text -> inconclusive, falls back to header_default") {
        LoadOptions o;
        o.headerDefault = false;
        auto t = load("x,y\nfoo,bar\n", o);
        CHECK(t->headerDetect == HeaderDetect::Inconclusive);
        CHECK_FALSE(t->header);
        o.headerDefault = true;
        CHECK(load("x,y\nfoo,bar\n", o)->header);
    }
    SUBCASE("date column") {
        auto t = load("when,what\n2024-01-01,a\n2024-02-01,b\n");
        CHECK(t->headerDetect == HeaderDetect::Yes);
        CHECK(t->columnType(0) == ValueKind::Date);
    }
    SUBCASE("forced modes win") {
        LoadOptions o;
        o.headerMode = HeaderMode::Off;
        CHECK_FALSE(load("name,age\nbob,12\n", o)->header);
        o.headerMode = HeaderMode::On;
        CHECK(load("1,2\n3,4\n", o)->header);
    }
    SUBCASE("thresholds") {
        std::string s = "n,v\nr,1\nr,2\nr,3\nr,x\n";  // 75% numeric
        LoadOptions o;
        auto t = load(s, o);
        CHECK(t->headerDetect == HeaderDetect::Yes);       // header threshold 75
        CHECK(t->columnType(1) == ValueKind::Text);        // type threshold 90
        o.typeThreshold = 75;
        o.headerThreshold = 80;
        t = load(s, o);
        CHECK(t->columnType(1) == ValueKind::Number);
        CHECK(t->headerDetect == HeaderDetect::Inconclusive);
    }
    SUBCASE("90% rule") {
        std::string s = "n,v\n";
        for (int i = 0; i < 9; ++i) s += "r," + std::to_string(i) + "\n";
        s += "r,oops\n";
        auto t = load(s);
        CHECK(t->columnType(1) == ValueKind::Number);  // 9 of 10
        s += "r,again\n";
        CHECK(load(s)->columnType(1) == ValueKind::Text);  // 9 of 11
    }
}

TEST_CASE("column types include row 0 when there is no header") {
    LoadOptions o;
    o.headerMode = HeaderMode::Off;
    auto t = load("a\nb\n1\n", o);
    CHECK(t->columnType(0) == ValueKind::Text);
}

TEST_CASE("own line counts per row and of the last cell") {
    auto t = load("a,\"x\ny\nz\"\n\"p\r\nq\",b\nc,d\n");
    REQUIRE(t->fileRows() == 3);
    CHECK(t->rowLines[0] == 3);
    CHECK(t->lastCellLines[0] == 3);
    CHECK(t->rowLines[1] == 2);
    CHECK(t->lastCellLines[1] == 1);
    CHECK(t->rowLines[2] == 1);
}

TEST_CASE("encodings are converted to UTF-8") {
    SUBCASE("UTF-8 BOM") {
        auto t = load("\xEF\xBB\xBFname,v\n");
        CHECK(t->encoding == Encoding::Utf8Bom);
        CHECK(t->cell(0, 0) == "name");
    }
    SUBCASE("UTF-16 LE with BOM") {
        std::string s("\xFF\xFE", 2);
        for (char16_t c : std::u16string(u"a,ж\n1,2\n")) {
            s.push_back(static_cast<char>(c & 0xFF));
            s.push_back(static_cast<char>(c >> 8));
        }
        auto t = load(s);
        CHECK(t->encoding == Encoding::Utf16LE);
        CHECK(t->cell(0, 1) == "\xD0\xB6");
        CHECK(t->fileRows() == 2);
    }
    SUBCASE("UTF-16 BE with BOM") {
        std::string s("\xFE\xFF", 2);
        for (char16_t c : std::u16string(u"x;y\n")) {
            s.push_back(static_cast<char>(c >> 8));
            s.push_back(static_cast<char>(c & 0xFF));
        }
        auto t = load(s);
        CHECK(t->encoding == Encoding::Utf16BE);
        CHECK(t->delimiter == ';');
        CHECK(t->cell(0, 1) == "y");
    }
    SUBCASE("invalid UTF-8 -> ANSI (Latin-1 fallback without a converter)") {
        auto t = load("caf\xE9,1\n");
        CHECK(t->encoding == Encoding::Ansi);
        CHECK(t->cell(0, 0) == "caf\xC3\xA9");
    }
    SUBCASE("forced ANSI uses the converter") {
        LoadOptions o;
        o.encoding = EncodingChoice::Ansi;
        auto t = csv::loadTable(std::string("ab"), o, [](std::string_view b) {
            std::u16string r;
            for (char c : b) r.push_back(static_cast<char16_t>(c == 'a' ? u'а' : c));
            return r;
        });
        CHECK(t->cell(0, 0) == "\xD0\xB0" "b");
    }
}

TEST_CASE("column letters") {
    CHECK(Table::columnLetters(0) == "A");
    CHECK(Table::columnLetters(25) == "Z");
    CHECK(Table::columnLetters(26) == "AA");
    CHECK(Table::columnLetters(701) == "ZZ");
    CHECK(Table::columnLetters(702) == "AAA");
}

TEST_CASE("longest cell candidates") {
    auto t = load("h\nx\nxxxxx\nxx\nxxx\n");
    REQUIRE(t->stats.size() == 1);
    auto& l = t->stats[0].longest;
    CHECK(l.size() == 4);
    auto it = std::max_element(l.begin(), l.end());
    CHECK(it->first == 5);
    CHECK(it->second == 2);
}

TEST_CASE("column clamp") {
    std::string row;
    for (size_t i = 0; i < kMaxColumns + 5; ++i) row += (i ? "," : "") + std::to_string(i % 10);
    auto t = load(row + "\n");
    CHECK(t->columnsClamped);
    CHECK(t->columns == kMaxColumns);
    CHECK(t->cell(0, kMaxColumns - 1).size() == 11);  // "3,4,5,6,7,8" style tail
}

TEST_CASE("typical length (percentile of cell lengths)") {
    std::string s = "h\n";
    for (int i = 1; i <= 99; ++i) s += std::to_string(i) + "\n";  // 9 one-digit, 90 two-digit
    s += "\"" + std::string(5000, 'x') + "\"\n";                      // one huge outlier
    s += "\n";                                                         // empty: ignored
    auto t = load(s);
    REQUIRE(t->header);
    auto p90 = t->typicalLength(0, 90);
    CHECK_FALSE(p90.empty);
    CHECK(p90.chars == 2);
    CHECK_FALSE(p90.beyondHistogram);
    REQUIRE_FALSE(p90.rows.empty());
    CHECK(t->cell(p90.rows[0], 0).size() == 2);
    CHECK(t->typicalLength(0, 5).chars == 1);
    auto p100 = t->typicalLength(0, 100);
    CHECK(p100.beyondHistogram);  // the outlier
    // no header: row 0 counts
    LoadOptions o;
    o.headerMode = HeaderMode::Off;
    auto n = load("abcdef\nx\n", o);
    CHECK(n->typicalLength(0, 100).chars == 6);
    CHECK(n->typicalLength(0, 50).chars == 1);
    // empty column
    auto e = load("a,b\n1,\n2,\n");
    CHECK(e->typicalLength(1, 90).empty);
    // multi-byte characters count once
    auto u = load("h\n\xD0\x96\xD0\x96\xD0\x96\n");
    CHECK(u->typicalLength(0, 90).chars == 3);
}
