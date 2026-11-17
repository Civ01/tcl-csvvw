#pragma once

#include <doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "core/IniDocument.h"
#include "csv/Parser.h"
#include "csv/Table.h"

namespace csvvw::test {

inline std::vector<std::string> splitValues(std::string_view data, char delim = ',') {
    auto starts = csv::scanRows(data, delim);
    std::vector<std::string> out;
    std::vector<csv::Field> f;
    for (size_t r = 0; r + 1 < starts.size(); ++r) {
        csv::splitRow(data, starts[r], starts[r + 1], delim, f);
        std::string row;
        for (size_t i = 0; i < f.size(); ++i) row += (i ? "|" : "") + csv::fieldValue(data, f[i]);
        out.push_back(row);
    }
    return out;
}

inline std::unique_ptr<csv::Table> load(std::string s, csv::LoadOptions o = {}) {
    return csv::loadTable(std::move(s), o);
}

inline std::filesystem::path samplePath(const char* name) {
    return std::filesystem::path(CSVVW_SOURCE_DIR) / "tests" / "samples" / name;
}

inline std::string readSample(const char* name) {
    std::string s;
    REQUIRE(readFileBytes(samplePath(name), s));
    return s;
}

}  // namespace csvvw::test
