// Number / date recognition for column typing and sorting (SPECS §9.1).
#pragma once

#include <string_view>

namespace csvvw::csv {

enum class ValueKind { Empty, Number, Date, Text };

// How a single ',' between digits is read when no '.' is present ("1,5").
enum class CommaMode { Thousands, Decimal };

// What a number's use of ',' says about the column's convention.
enum class CommaHint { None, Decimal, Thousands };

// Number: optional sign, digits with optional thousands separators (space,
// NBSP, '\'', and ',' or '.' depending on the convention), an optional decimal
// part, optional exponent and an optional trailing '%'. Surrounding spaces are
// ignored. `hint` reports an unambiguous comma convention ("3,14" -> Decimal,
// "1,234.5" -> Thousands).
bool parseNumber(std::string_view s, CommaMode mode, double& out, CommaHint* hint = nullptr);

// Dates: ISO "YYYY-MM-DD" with optional "[T ]hh:mm[:ss[.fff]]" and optional
// trailing "Z"; "DD.MM.YYYY" with optional " hh:mm[:ss]". The key is seconds
// since 0001-01-01 (comparable, not a real epoch).
bool parseDate(std::string_view s, double& out);

bool isBlank(std::string_view s);

// Classifies a value (Number takes precedence over Date).
ValueKind classify(std::string_view s, CommaMode mode, double* key = nullptr, CommaHint* hint = nullptr);

}  // namespace csvvw::csv
