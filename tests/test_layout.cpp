#include <doctest.h>

#include "csv/TextLayout.h"

using namespace csvvw::csv;

namespace {
// Monospace measure: every character is 10 px wide.
size_t fit10(std::u16string_view s, int width) { return std::min(s.size(), static_cast<size_t>(width / 10)); }

std::vector<std::u16string> lines(const WrapResult& r) {
    std::vector<std::u16string> out;
    for (auto l : r.lines) out.emplace_back(l);
    return out;
}
}  // namespace

TEST_CASE("single-line display transform") {
    auto d = singleLine(u"a\r\nb\nc\rd\te");
    CHECK(d.text == u"a↵b↵c↵d e");
    REQUIRE(d.hasGlyph());
    CHECK(d.glyphMask[1]);
    CHECK_FALSE(d.glyphMask[0]);
    CHECK_FALSE(singleLine(u"plain").hasGlyph());
}

TEST_CASE("own lines keep a trailing glyph") {
    auto l = ownLines(u"one\r\ntwo\nthree");
    REQUIRE(l.size() == 3);
    CHECK(l[0].text == u"one↵");
    CHECK(l[1].text == u"two↵");
    CHECK(l[2].text == u"three");
    CHECK(countLines(u"one\r\ntwo\nthree") == 3);
    CHECK(countLines(u"x") == 1);
    CHECK(hasLineBreak(u"a\rb"));
    CHECK_FALSE(hasLineBreak(u"ab"));
}

TEST_CASE("offset mapping through CRLF collapse") {
    std::u16string s = u"ab\r\ncd";
    CHECK(singleLineOffset(s, 0) == 0);
    CHECK(singleLineOffset(s, 2) == 2);
    CHECK(singleLineOffset(s, 4) == 3);  // 'c' follows the single glyph
    CHECK(singleLineOffset(s, 6) == 5);
}

TEST_CASE("word wrap") {
    SUBCASE("fits") {
        auto r = wrapText(u"short", 100, 8, fit10);
        CHECK(lines(r) == std::vector<std::u16string>{u"short"});
        CHECK_FALSE(r.truncated);
    }
    SUBCASE("breaks at spaces, drops them") {
        auto r = wrapText(u"the quick brown fox", 100, 8, fit10);  // 10 chars per line
        CHECK(lines(r) == std::vector<std::u16string>{u"the quick", u"brown fox"});
    }
    SUBCASE("breaks after punctuation") {
        auto r = wrapText(u"alpha-beta/gamma", 80, 8, fit10);
        CHECK(lines(r) == std::vector<std::u16string>{u"alpha-", u"beta/", u"gamma"});
    }
    SUBCASE("long word is split between characters") {
        auto r = wrapText(u"abcdefghijkl", 50, 8, fit10);
        CHECK(lines(r) == std::vector<std::u16string>{u"abcde", u"fghij", u"kl"});
    }
    SUBCASE("line cap") {
        auto r = wrapText(u"aa bb cc dd ee", 20, 3, fit10);
        CHECK(lines(r) == std::vector<std::u16string>{u"aa", u"bb", u"cc"});
        CHECK(r.truncated);
    }
    SUBCASE("zero width still progresses") {
        auto r = wrapText(u"abc", 0, 10, fit10);
        CHECK(r.lines.size() == 3);
    }
    SUBCASE("empty") {
        auto r = wrapText(u"", 50, 3, fit10);
        CHECK(r.lines.size() == 1);
    }
}
