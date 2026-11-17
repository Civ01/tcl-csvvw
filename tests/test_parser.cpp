#include "TestUtil.h"

using namespace csvvw;
using csvvw::test::splitValues;

TEST_CASE("parser: simple rows and line endings") {
    CHECK(splitValues("a,b,c\r\n1,2,3\r\n") == std::vector<std::string>{"a|b|c", "1|2|3"});
    CHECK(splitValues("a,b\n1,2") == std::vector<std::string>{"a|b", "1|2"});
    CHECK(splitValues("a,b\r1,2\r") == std::vector<std::string>{"a|b", "1|2"});
    CHECK(splitValues("a\r\nb\nc\rd") == std::vector<std::string>{"a", "b", "c", "d"});
    CHECK(splitValues("") .empty());
    CHECK(splitValues("\n") == std::vector<std::string>{""});
}

TEST_CASE("parser: trailing terminator does not add a row, blank lines do") {
    CHECK(splitValues("a\n").size() == 1);
    CHECK(splitValues("a\n\nb\n") == std::vector<std::string>{"a", "", "b"});
}

TEST_CASE("parser: empty fields") {
    CHECK(splitValues(",,") == std::vector<std::string>{"||"});
    CHECK(splitValues("a,,c\n,b,") == std::vector<std::string>{"a||c", "|b|"});
}

TEST_CASE("parser: quoted fields") {
    CHECK(splitValues("\"a,b\",c") == std::vector<std::string>{"a,b|c"});
    CHECK(splitValues("\"he said \"\"hi\"\"\",x") == std::vector<std::string>{"he said \"hi\"|x"});
    CHECK(splitValues("\"\",x") == std::vector<std::string>{"|x"});
    CHECK(splitValues("\"line1\r\nline2\",b\r\nnext,row") ==
          std::vector<std::string>{"line1\r\nline2|b", "next|row"});
    CHECK(splitValues("\"a\nb\nc\"") == std::vector<std::string>{"a\nb\nc"});
}

TEST_CASE("parser: lenient cases") {
    // stray quote inside an unquoted field is literal
    CHECK(splitValues("ab\"c,d") == std::vector<std::string>{"ab\"c|d"});
    // text after the closing quote is appended verbatim
    CHECK(splitValues("\"ab\"cd,e") == std::vector<std::string>{"abcd|e"});
    CHECK(splitValues("\"ab\" \"x\",e") == std::vector<std::string>{"ab \"x\"|e"});
    // unterminated quote runs to the end
    CHECK(splitValues("a,\"bc\nd,e") == std::vector<std::string>{"a|bc\nd,e"});
    // UE-style values with quotes in unquoted fields
    CHECK(splitValues("N,(ProfileName=\"Bad\",ToolTip=\"x y\"),P") ==
          std::vector<std::string>{"N|(ProfileName=\"Bad\"|ToolTip=\"x y\")|P"});
}

TEST_CASE("parser: other delimiters") {
    CHECK(splitValues("a;b;\"c;d\"", ';') == std::vector<std::string>{"a|b|c;d"});
    CHECK(splitValues("a\tb\tc", '\t') == std::vector<std::string>{"a|b|c"});
    CHECK(splitValues("a|b,c", '|') == std::vector<std::string>{"a|b,c"});
}

TEST_CASE("parser: maxFields makes the last field absorb the rest") {
    std::string d = "a,b,c,d\n";
    std::vector<csv::Field> f;
    csv::splitRow(d, 0, static_cast<uint32_t>(d.size()), ',', f, 2);
    REQUIRE(f.size() == 2);
    CHECK(csv::fieldValue(d, f[0]) == "a");
    CHECK(csv::fieldValue(d, f[1]) == "b,c,d");
}

TEST_CASE("parser: UTF-8 content is passed through") {
    CHECK(splitValues("π,\"ж,ы\",日本") == std::vector<std::string>{"π|ж,ы|日本"});
}
