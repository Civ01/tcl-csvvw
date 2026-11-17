// RFC 4180 CSV scanning on UTF-8 bytes (SPECS §5.2, §5.4).
//
// The parser never copies data: the row index holds byte offsets into the
// decoded buffer, fields are split per row on demand and only quoted fields
// need unescaping.
//
// Lenient rules (never errors):
//  - a '"' starts a quoted field only as the first byte of the field;
//    elsewhere it is literal text,
//  - inside quotes "" is a literal quote; a single " closes the quotes and any
//    text after it up to the next delimiter is appended verbatim,
//  - an unterminated quote runs to the end of the data,
//  - CRLF, LF and lone CR end a row (outside quotes).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace csvvw::csv {

// One field of a row: [offset, offset+length) in the buffer. A quoted field's
// span starts at its opening quote and must be unescaped (see unescape()).
struct Field {
    uint32_t offset = 0;
    uint32_t length = 0;
};

// Byte offsets where each row starts, plus a final sentinel (= data size), so
// row r spans [starts[r], starts[r+1]) including its line terminator.
// A trailing line terminator at the end of the data does not start a new row.
std::vector<uint32_t> scanRows(std::string_view data, char delimiter);

// Splits the row [begin, end) into fields (appended to `out` after clearing).
// Stops at the first line terminator outside quotes. An empty row yields one
// empty field. With maxFields > 0, the last field absorbs the rest of the row.
void splitRow(std::string_view data, uint32_t begin, uint32_t end, char delimiter, std::vector<Field>& out,
              size_t maxFields = 0);

inline bool isQuoted(std::string_view data, const Field& f) {
    return f.length > 0 && data[f.offset] == '"';
}

// Raw bytes of the field (for unquoted fields this is the value).
inline std::string_view rawField(std::string_view data, const Field& f) { return data.substr(f.offset, f.length); }

// Value of a field: unquoted fields are returned as is, quoted ones unescaped.
std::string fieldValue(std::string_view data, const Field& f);
// Unescapes a quoted field span (starting with its opening quote).
std::string unescape(std::string_view quotedSpan);

}  // namespace csvvw::csv
