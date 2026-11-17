#include "TestUtil.h"

#include "core/TextUtil.h"
#include "csv/Ops.h"

using namespace csvvw;
using namespace csvvw::csv;
using csvvw::test::load;

namespace {
std::vector<std::string> column(const Table& t, const std::vector<uint32_t>& order, size_t col) {
    std::vector<std::string> out;
    for (auto d : order) out.push_back(t.cell(t.fileRowOfData(d), col));
    return out;
}
std::vector<uint32_t> identity(const Table& t) {
    std::vector<uint32_t> o(t.dataRows());
    for (size_t i = 0; i < o.size(); ++i) o[i] = static_cast<uint32_t>(i);
    return o;
}
}  // namespace

TEST_CASE("sort: numbers, empties last, unparsed after parsed") {
    auto t = load("k,v\na,10\nb,\nc,9\nd,x\ne,-1\nf,100\n");
    REQUIRE(t->header);
    REQUIRE(t->columnType(1) == ValueKind::Text);  // 4 of 5 numeric: below 90%
    auto asc = sortRows(*t, 1, SortDir::Asc, SortAs::Number, {});
    CHECK(column(*t, asc, 1) == std::vector<std::string>{"-1", "9", "10", "100", "x", ""});
    auto desc = sortRows(*t, 1, SortDir::Desc, SortAs::Number, {});
    CHECK(column(*t, desc, 1) == std::vector<std::string>{"100", "10", "9", "-1", "x", ""});
    auto text = sortRows(*t, 1, SortDir::Asc, SortAs::Text, {});
    // portable key: natural digit order, punctuation after digits
    CHECK(column(*t, text, 1) == std::vector<std::string>{"9", "10", "100", "-1", "x", ""});
    CHECK(sortRows(*t, 1, SortDir::None, SortAs::Auto, {}) == identity(*t));
}

TEST_CASE("sort: auto type, stable, natural text") {
    auto t = load("name,n\nfile10,1\nFile2,1\nfile1,2\nalpha,1\n");
    auto byName = sortRows(*t, 0, SortDir::Asc, SortAs::Auto, {});
    CHECK(column(*t, byName, 0) == std::vector<std::string>{"alpha", "file1", "File2", "file10"});
    auto byN = sortRows(*t, 1, SortDir::Asc, SortAs::Auto, {});  // stable: original order among equal keys
    CHECK(column(*t, byN, 0) == std::vector<std::string>{"file10", "File2", "alpha", "file1"});
    auto byNDesc = sortRows(*t, 1, SortDir::Desc, SortAs::Auto, {});
    CHECK(column(*t, byNDesc, 0) == std::vector<std::string>{"file1", "file10", "File2", "alpha"});
}

TEST_CASE("sort: dates and custom key function") {
    auto t = load("d\n02.01.2024\n2023-12-31\n2024-01-01 12:00\n");
    REQUIRE(t->columnType(0) == ValueKind::Date);
    auto o = sortRows(*t, 0, SortDir::Asc, SortAs::Auto, {});
    CHECK(column(*t, o, 0) == std::vector<std::string>{"2023-12-31", "2024-01-01 12:00", "02.01.2024"});
    // reverse-alphabetical key function proves the callback is used
    auto rev = [](std::u16string_view s) {
        std::string k;
        for (char16_t c : s) k.push_back(static_cast<char>(255 - (c & 0xFF)));
        return k;
    };
    LoadOptions lo;
    lo.headerMode = HeaderMode::On;
    auto t2 = load("h\nb\na\nc\n", lo);
    auto o2 = sortRows(*t2, 0, SortDir::Asc, SortAs::Text, rev);
    CHECK(column(*t2, o2, 0) == std::vector<std::string>{"c", "b", "a"});
}

TEST_CASE("sort: comma decimal column") {
    auto t = load("v\n\"1,5\"\n\"10,25\"\n\"2,75\"\n");
    CHECK(t->commaMode(0) == CommaMode::Decimal);
    auto o = sortRows(*t, 0, SortDir::Asc, SortAs::Auto, {});
    CHECK(column(*t, o, 0) == std::vector<std::string>{"1,5", "2,75", "10,25"});
}

TEST_CASE("default sort key") {
    CHECK(defaultSortKey(u"a2") < defaultSortKey(u"a10"));
    CHECK(defaultSortKey(u"A") == defaultSortKey(u"a"));
    CHECK(defaultSortKey(u"a007") == defaultSortKey(u"a7"));
    CHECK(defaultSortKey(u"9") < defaultSortKey(u"a"));
}

TEST_CASE("search") {
    auto t = load("name,city\nbob,Paris\nann,paris\nzed,Rome\n");
    auto order = identity(*t);
    auto m = defaultMatcher(u"paris", false, false);
    SearchStart st{-1, 0, 0, true};
    auto h = searchTable(*t, order, st, false, m);
    REQUIRE(h);
    CHECK(h->row == 0);
    CHECK(h->col == 1);
    // find next: from just after the current cell
    h = searchTable(*t, order, SearchStart{h->row, h->col, 0, false}, false, m);
    REQUIRE(h);
    CHECK(h->row == 1);
    CHECK_FALSE(searchTable(*t, order, SearchStart{h->row, h->col, 0, false}, false, m));
    // case-sensitive
    auto mc = defaultMatcher(u"paris", true, false);
    h = searchTable(*t, order, st, false, mc);
    REQUIRE(h);
    CHECK(h->row == 1);
    // backwards from the last cell
    h = searchTable(*t, order, SearchStart{2, 1, 1000, true}, true, m);
    REQUIRE(h);
    CHECK(h->row == 1);
    // header row is searched
    h = searchTable(*t, order, st, false, defaultMatcher(u"city", false, false));
    REQUIRE(h);
    CHECK(h->row == -1);
    // respects sort order
    std::vector<uint32_t> rev{2, 1, 0};
    h = searchTable(*t, rev, SearchStart{0, 0, 0, true}, false, m);
    REQUIRE(h);
    CHECK(h->row == 1);  // display row 1 = "ann,paris"
}

TEST_CASE("search: whole words and in-cell positions") {
    auto t = load("x\nthe cat scattered cats\n", LoadOptions{EncodingChoice::Auto, 0, HeaderMode::Off});
    auto order = identity(*t);
    auto m = defaultMatcher(u"cat", false, true);
    auto h = searchTable(*t, order, SearchStart{0, 0, 0, true}, false, m);
    REQUIRE(h);
    CHECK(h->row == 1);
    CHECK(h->pos == 4);
    h = searchTable(*t, order, SearchStart{1, 0, h->pos + 1, true}, false, m);
    CHECK_FALSE(h);  // "scattered" and "cats" are not whole words
    auto any = defaultMatcher(u"cat", false, false);
    h = searchTable(*t, order, SearchStart{1, 0, 5, true}, false, any);
    REQUIRE(h);
    CHECK(h->pos == 9);   // "scattered"
    h = searchTable(*t, order, SearchStart{1, 0, 9, true}, true, any);  // backward, before 9
    REQUIRE(h);
    CHECK(h->pos == 4);
}

TEST_CASE("search: full text beyond the display cap, non-ASCII") {
    std::string big(100000, 'a');
    auto t = load("h\n\"" + big + "\xD0\x96needle\"\n");
    auto h = searchTable(*t, identity(*t), SearchStart{0, 0, 0, true}, false, defaultMatcher(u"Жneedle", true, false));
    REQUIRE(h);
    CHECK(h->pos == 100000);
}

TEST_CASE("clipboard") {
    auto t = load("a,b,c\n1,\"x\ty\",3\n4,\"q\"\"r\",6\n7,\"m\nn\",9\n");
    REQUIRE(t->header);
    auto order = identity(*t);
    SUBCASE("single cell raw") {
        CHECK(copyRange(*t, order, CellRange{0, 0, 1, 1}, CopyFormat::Tsv, false) == "x\ty");
    }
    SUBCASE("TSV quoting") {
        CHECK(copyRange(*t, order, CellRange{0, 2, 0, 1}, CopyFormat::Tsv, false) ==
              "1\t\"x\ty\"\r\n4\t\"q\"\"r\"\r\n7\t\"m\nn\"");
    }
    SUBCASE("with header") {
        CHECK(copyRange(*t, order, CellRange{0, 0, 1, 2}, CopyFormat::Tsv, true) == "b\tc\r\n\"x\ty\"\t3");
    }
    SUBCASE("CSV uses the source delimiter") {
        CHECK(copyRange(*t, order, CellRange{1, 1, 0, 2}, CopyFormat::Csv, false) == "4,\"q\"\"r\",6");
        CHECK(copyRange(*t, order, CellRange{0, 0, 1, 1}, CopyFormat::Csv, false) == "x\ty");
    }
    SUBCASE("display order and reversed range") {
        std::vector<uint32_t> rev{2, 1, 0};
        CHECK(copyRange(*t, rev, CellRange{1, 0, 2, 2}, CopyFormat::Tsv, false) == "9\r\n6");
    }
    SUBCASE("padded cells") {
        auto r = load("a,b\n1\n", LoadOptions{EncodingChoice::Auto, 0, HeaderMode::Off});
        CHECK(copyRange(*r, identity(*r), CellRange{0, 1, 0, 1}, CopyFormat::Tsv, false) == "a\tb\r\n1\t");
    }
}
