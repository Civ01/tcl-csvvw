// Minimal, round-trippable INI document (UTF-8 strings).
//
// - Sections and keys are case-insensitive; order and duplicates are kept.
// - Only full-line comments ("; ..." or "# ...") are recognised, so values may
//   contain ';' and '#' (needed for colors and regexes).
// - Keys before the first section belong to section "".
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "TextDecoder.h"

namespace csvvw {

class IniDocument {
public:
    using Entry = std::pair<std::string, std::string>;

    static IniDocument parse(std::string_view utf8);
    static IniDocument parse(std::u16string_view text);
    // Returns false if the file cannot be read (document is left empty).
    static bool loadFile(const std::filesystem::path& p, IniDocument& out, const AnsiConverter& ansi = {});

    std::optional<std::string> get(std::string_view section, std::string_view key) const;
    std::string getOr(std::string_view section, std::string_view key, std::string_view def) const;
    std::vector<Entry> entries(std::string_view section) const;
    std::vector<std::string> sections() const;  // in order, as written
    bool hasSection(std::string_view section) const;

    // Updates the first occurrence of key, or appends it to the section
    // (creating the section at the end of the document if needed).
    void set(std::string_view section, std::string_view key, std::string_view value);

    // Removes every occurrence of key in section. Returns true if any.
    bool remove(std::string_view section, std::string_view key);
    // Removes the section header and all of its lines (comments directly
    // belonging to it included) up to the next section header.
    bool removeSection(std::string_view section);

    std::string serialize() const;  // UTF-8, CRLF line endings
    // Writes in the encoding the file was loaded with (UTF-8 with/without BOM,
    // UTF-16 LE/BE with BOM). ANSI files are written as UTF-8 (still readable:
    // valid UTF-8 is detected first).
    bool saveFile(const std::filesystem::path& p) const;
    Encoding encoding() const { return encoding_; }
    void setEncoding(Encoding e) { encoding_ = e; }

private:
    enum class LineType { Blank, Comment, Section, KeyValue, Other };
    struct Line {
        LineType type = LineType::Other;
        std::string raw;      // original text for Blank/Comment/Other
        std::string section;  // section name (for Section lines: the name itself)
        std::string key;
        std::string value;
    };
    std::vector<Line> lines_;
    Encoding encoding_ = Encoding::Utf8;
};

bool readFileBytes(const std::filesystem::path& p, std::string& out);

}  // namespace csvvw
