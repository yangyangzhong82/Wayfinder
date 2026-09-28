#include "wayfinder/Locale.h"
#include <algorithm>
#include <cctype>

namespace wayfinder {
namespace {
constexpr Translation entries[]{
#define WF_TEXT(key, en, zh) {key, en, zh},
#include "wayfinder/Translations.inc"
#undef WF_TEXT
};
} // namespace
std::span<Translation const> translationCatalog() { return entries; }
void                         Locale::select(std::string_view setting, std::string_view gameLanguage) {
    std::string language(setting == "auto" ? gameLanguage : setting);
    std::transform(language.begin(), language.end(), language.begin(), [](unsigned char ch) {
        return ch == '-' ? '_' : static_cast<char>(std::tolower(ch));
    });
    mChinese = language == "zh_cn" || language == "zh_sg" || language == "zh_hans" || language.starts_with("zh_hans_");
}
std::string Locale::tr(std::string_view key) const {
    for (auto const& entry : entries)
        if (entry.key == key) return std::string(mChinese ? entry.simplifiedChinese : entry.english);
    return std::string(key);
}
std::string Locale::error(std::string_view key) const {
    for (auto const& entry : entries)
        if (entry.key == key) return tr(key);
    return tr("Operation failed; see log for details");
}
std::string Locale::biome(std::string_view identifier) const {
    auto id = identifier;
    if (id.starts_with("minecraft:")) id.remove_prefix(10);
    auto key        = "biome." + std::string(id);
    auto translated = tr(key);
    return translated == key ? std::string(identifier) : translated;
}
} // namespace wayfinder
