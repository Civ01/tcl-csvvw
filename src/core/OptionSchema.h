// Declarative description of every [General] option (portable).
//
// Config::fromIni, the settings window and Settings::save() all go through
// this table, so adding an option means adding one entry here plus a field in
// Config.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "Config.h"

namespace csvvw {

enum class OptionType { Bool, Int, Size, Enum, Font, List, Char };

struct OptionDef {
    const char* key;
    OptionType type;
    const char* label;
    const char* defaultValue;
    long long minValue = 0, maxValue = 0;    // Int / Size (Size: bytes)
    std::vector<const char*> choices = {};   // Enum
    std::function<std::string(const Config&)> get;
    std::function<bool(Config&, std::string_view)> set;  // false if invalid

    // Canonical form ("104857600" -> "100M", "yes" -> "true"); empty if invalid.
    std::string normalize(std::string_view value) const;
    bool sameValue(std::string_view a, std::string_view b) const;
};

const std::vector<OptionDef>& generalOptions();
const OptionDef* findGeneralOption(std::string_view key);

std::string getGeneralValue(const Config& cfg, std::string_view key);
bool setGeneralValue(Config& cfg, std::string_view key, std::string_view value);

bool parseBool(std::string_view s, bool& out);

}  // namespace csvvw
