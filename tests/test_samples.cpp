// Real-world sample files (tests/samples).
#include "TestUtil.h"

#include "csv/Ops.h"

using namespace csvvw;
using namespace csvvw::csv;
using csvvw::test::readSample;

TEST_CASE("sample: UE_cvars.csv (Unreal Engine console variables dump)") {
    auto t = loadTable(readSample("UE_cvars.csv"), {});
    CHECK(t->encoding == Encoding::Utf8);
    CHECK(t->delimiter == ',');
    CHECK(t->fileRows() == 7035);
    CHECK(t->columns == 18);  // ragged: unquoted commas inside some values
    CHECK(t->header);         // all-text table: header_default
    CHECK(t->headerDetect == HeaderDetect::Inconclusive);
    CHECK(t->caption(0) == "NAME");
    CHECK(t->caption(2) == "SETBY");
    CHECK(t->cell(1, 0) == "a.AnimNode.AimOffsetLookAt.Debug");
    CHECK(t->cell(1418, 1) == "(Default=65536");
    CHECK(t->cell(1693, 1) == "(ProfileName=\"Bad\"");
    CHECK(t->cell(4552, 17) == "Constructor");
    CHECK(t->columnType(1) == ValueKind::Text);  // ~83% numeric
    for (auto l : t->rowLines) CHECK(l == 1);

    // sort by VALUE as number: numbers first, ascending
    auto o = sortRows(*t, 1, SortDir::Asc, SortAs::Number, {});
    double prev = -1e300, v;
    bool inNumbers = true;
    for (auto d : o) {
        auto s = t->cell(t->fileRowOfData(d), 1);
        if (parseNumber(s, CommaMode::Thousands, v)) {
            CHECK(inNumbers);
            CHECK(v >= prev);
            prev = v;
        } else {
            inNumbers = false;
        }
    }
    auto order = sortRows(*t, 0, SortDir::None, SortAs::Auto, {});
    auto hit = searchTable(*t, order, SearchStart{-1, 0, 0, true}, false, defaultMatcher(u"nanite.visualize", false, false));
    REQUIRE(hit);
    CHECK(t->cell(t->fileRowOfData(order[static_cast<size_t>(hit->row)]), hit->col).rfind("r.Nanite.Visualize", 0) == 0);
}

TEST_CASE("sample: UTF-16 semicolon files") {
    for (const char* name : {"utf16le_bom.csv", "utf16be_bom.csv"}) {
        INFO(name);
        auto t = loadTable(readSample(name), {});
        CHECK(t->delimiter == ';');
        CHECK(t->fileRows() == 4);
        CHECK(t->columns == 3);
        CHECK(t->header);
        CHECK(t->headerDetect == HeaderDetect::Yes);
        CHECK(t->columnType(1) == ValueKind::Number);  // "13 010 112"
        CHECK(t->columnType(2) == ValueKind::Date);
        CHECK(t->cell(1, 0) == "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0");  // Москва
        auto o = sortRows(*t, 1, SortDir::Desc, SortAs::Auto, {});
        CHECK(t->cell(t->fileRowOfData(o[0]), 1) == "13 010 112");
    }
}

TEST_CASE("sample: cp1251 is detected as ANSI") {
    auto t = loadTable(readSample("cp1251.csv"), {});
    CHECK(t->encoding == Encoding::Ansi);
    CHECK(t->delimiter == ';');
    CHECK(t->fileRows() == 4);
}

TEST_CASE("sample: multiline cells") {
    auto t = loadTable(readSample("multiline.csv"), {});
    CHECK(t->fileRows() == 5);
    CHECK(t->header);
    CHECK(t->cell(1, 2) == "first line\r\nsecond line");
    CHECK(t->cell(2, 1) == "Beta \"quoted\"");
    CHECK(t->rowLines[3] == 4);
    CHECK(t->lastCellLines[3] == 4);
    CHECK(t->lastCellLines[4] == 1);
}

TEST_CASE("sample: TSV and header-less numbers") {
    LoadOptions o;
    o.preferTab = true;
    auto t = loadTable(readSample("tabs.tsv"), o);
    CHECK(t->delimiter == '\t');
    CHECK(t->header);
    CHECK(t->columnType(1) == ValueKind::Number);
    CHECK(t->columnType(2) == ValueKind::Date);
    auto n = loadTable(readSample("numbers_noheader.csv"), {});
    CHECK_FALSE(n->header);
    CHECK(n->headerDetect == HeaderDetect::No);
    CHECK(n->dataRows() == 3);
}
