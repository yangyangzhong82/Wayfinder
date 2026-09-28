#pragma once
#include <span>
#include <string>
#include <string_view>

namespace wayfinder {
struct Translation {
    std::string_view key, english, simplifiedChinese;
};
std::span<Translation const> translationCatalog();
class Locale {
public:
    // Unknown game languages fall back to English; OS language is never consulted.
    void             select(std::string_view setting, std::string_view gameLanguage);
    std::string      tr(std::string_view key) const;
    std::string      error(std::string_view key) const;
    std::string      biome(std::string_view identifier) const;
    bool             chinese() const { return mChinese; }
    std::string_view code() const { return mChinese ? "zh_CN" : "en_US"; }

private:
    bool mChinese{};
};
} // namespace wayfinder
