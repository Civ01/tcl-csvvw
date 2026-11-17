// Settings model behind the settings window and the context-menu toggles.
//
// Holds the parsed IniDocument plus a Config that the UI edits. save() writes
// only what changed (values compared in canonical form, so "100M" ==
// "104857600"), keeping comments, ordering and unknown keys intact.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "Config.h"
#include "IniDocument.h"

namespace csvvw {

class Settings {
public:
    // Returns false if the file could not be read (defaults are used then and
    // save() creates the file).
    bool load(const std::filesystem::path& iniPath, const AnsiConverter& ansi = {});
    bool save();
    void applyToDocument();

    Config& config() { return config_; }
    const Config& config() const { return config_; }
    const IniDocument& document() const { return doc_; }
    const std::filesystem::path& path() const { return path_; }

    // Re-reads the file, changes one [General] option and writes it back
    // (used by the context-menu toggles). Unchanged values are not rewritten.
    static bool saveGeneralValue(const std::filesystem::path& iniPath, std::string_view key, std::string_view value,
                                 const AnsiConverter& ansi = {});

private:
    std::filesystem::path path_;
    IniDocument doc_;
    Config config_ = Config::defaults();
};

}  // namespace csvvw
