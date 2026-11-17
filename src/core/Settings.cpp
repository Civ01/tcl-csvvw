#include "Settings.h"

#include "OptionSchema.h"
#include "TextUtil.h"

namespace csvvw {

bool Settings::load(const std::filesystem::path& iniPath, const AnsiConverter& ansi) {
    path_ = iniPath;
    doc_ = IniDocument();
    bool ok = IniDocument::loadFile(iniPath, doc_, ansi);
    config_ = ok ? Config::fromIni(doc_) : Config::defaults();
    return ok;
}

void Settings::applyToDocument() {
    for (const auto& o : generalOptions()) {
        std::string now = o.get(config_);
        auto old = doc_.get(kGeneral, o.key);
        if (old ? !o.sameValue(*old, now) : !o.sameValue(o.defaultValue, now)) doc_.set(kGeneral, o.key, now);
    }
    for (bool dark : {false, true}) {
        const char* section = dark ? kColorsDark : kColorsLight;
        const auto& map = dark ? config_.colorsDark : config_.colorsLight;
        for (const auto& k : colorKeys()) {
            auto it = map.find(k.key);
            std::string want = it == map.end() ? std::string() : it->second;
            uint32_t v;
            if (!want.empty() && parseColor(want, v)) want = formatColor(v);
            auto old = doc_.get(section, k.key);
            std::string oldNorm = old ? std::string(trim(*old)) : std::string();
            if (!oldNorm.empty() && parseColor(oldNorm, v)) oldNorm = formatColor(v);
            if (oldNorm == want) continue;
            if (!old && want.empty()) continue;
            doc_.set(section, k.key, want);
        }
    }
}

bool Settings::save() {
    applyToDocument();
    return doc_.saveFile(path_);
}

bool Settings::saveGeneralValue(const std::filesystem::path& iniPath, std::string_view key, std::string_view value,
                                const AnsiConverter& ansi) {
    auto* o = findGeneralOption(key);
    if (!o) return false;
    IniDocument doc;
    IniDocument::loadFile(iniPath, doc, ansi);
    auto old = doc.get(kGeneral, o->key);
    if (old && o->sameValue(*old, value)) return true;
    doc.set(kGeneral, o->key, o->normalize(value).empty() ? std::string(value) : o->normalize(value));
    return doc.saveFile(iniPath);
}

}  // namespace csvvw
