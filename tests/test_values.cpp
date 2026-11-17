#include <doctest.h>

#include "csv/ValueParse.h"

using namespace csvvw::csv;

static bool num(const char* s, double expect, CommaMode m = CommaMode::Thousands) {
    double v = 0;
    if (!parseNumber(s, m, v)) return false;
    return v == doctest::Approx(expect);
}
static bool notNum(const char* s, CommaMode m = CommaMode::Thousands) {
    double v;
    return !parseNumber(s, m, v);
}

TEST_CASE("numbers") {
    CHECK(num("0", 0));
    CHECK(num("42", 42));
    CHECK(num("-3.5", -3.5));
    CHECK(num("+7", 7));
    CHECK(num(".5", 0.5));
    CHECK(num("0.200000", 0.2));
    CHECK(num("1e3", 1000));
    CHECK(num("2.5E-2", 0.025));
    CHECK(num("  12  ", 12));
    CHECK(num("50%", 50));
    CHECK(num("1,234,567", 1234567));
    CHECK(num("1,234.5", 1234.5));
    CHECK(num("1.234,5", 1234.5));
    CHECK(num("1 234 567", 1234567));
    CHECK(num("1'234", 1234));
    CHECK(num("1\xC2\xA0" "234,5", 1234.5));
    CHECK(num("3,14", 3.14));                        // unambiguous decimal comma
    CHECK(num("1,234", 1234));                       // ambiguous: thousands by default
    CHECK(num("1,234", 1.234, CommaMode::Decimal));  // column convention: decimal
    CHECK(num("1.234.567", 1234567));
}

TEST_CASE("not numbers") {
    CHECK(notNum(""));
    CHECK(notNum("-"));
    CHECK(notNum("abc"));
    CHECK(notNum("12abc"));
    CHECK(notNum("1.2.3"));
    CHECK(notNum("10.0.0.1"));
    CHECK(notNum("12."));
    CHECK(notNum("1,23,4"));
    CHECK(notNum("1e"));
    CHECK(notNum("0x10"));
    CHECK(notNum("2024-01-01"));
    CHECK(notNum("12.03.2024"));
    CHECK(notNum("(X=0.2"));
    CHECK(notNum("inf"));
}

TEST_CASE("comma hints") {
    double v;
    CommaHint h;
    parseNumber("3,14", CommaMode::Thousands, v, &h);
    CHECK(h == CommaHint::Decimal);
    parseNumber("1,234.5", CommaMode::Thousands, v, &h);
    CHECK(h == CommaHint::Thousands);
    parseNumber("1,234", CommaMode::Thousands, v, &h);
    CHECK(h == CommaHint::None);
}

TEST_CASE("dates") {
    double a, b, c;
    CHECK(parseDate("2024-01-31", a));
    CHECK(parseDate("2024-02-01", b));
    CHECK(a < b);
    CHECK(parseDate("01.02.2024", c));
    CHECK(c == b);
    CHECK(parseDate("2024-02-01T10:30", a));
    CHECK(a > b);
    CHECK(parseDate("2024-02-01 10:30:15.25Z", a));
    CHECK(parseDate("01.02.2024 23:59:59", a));
    CHECK(parseDate("2024-02-29", a));   // leap year
    CHECK_FALSE(parseDate("2023-02-29", a));
    CHECK_FALSE(parseDate("2024-13-01", a));
    CHECK_FALSE(parseDate("02/01/2024", a));  // US style is text
    CHECK_FALSE(parseDate("2024-01-01 25:00", a));
    CHECK_FALSE(parseDate("2024-01-01x", a));
}

TEST_CASE("classify") {
    CHECK(classify("", CommaMode::Thousands) == ValueKind::Empty);
    CHECK(classify("   ", CommaMode::Thousands) == ValueKind::Empty);
    CHECK(classify("12", CommaMode::Thousands) == ValueKind::Number);
    CHECK(classify("2024-05-06", CommaMode::Thousands) == ValueKind::Date);
    CHECK(classify("Constructor", CommaMode::Thousands) == ValueKind::Text);
}
