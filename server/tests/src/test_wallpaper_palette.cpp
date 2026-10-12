#include "tests/includes/test_framework.h"
#include "utils/wallpaper_palette.h"
#include "engine/local_modes/emoji_query.h"
#include "ipc/candidate_selection_policy.h"
#include <algorithm>
#include <cmath>

namespace
{
double Luminance(std::uint32_t rgb)
{
    const auto channel = [](unsigned byte) {
        const double value = byte / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel((rgb >> 16) & 255) + 0.7152 * channel((rgb >> 8) & 255) + 0.0722 * channel(rgb & 255);
}
double Contrast(std::uint32_t a, std::uint32_t b)
{
    const double first = Luminance(a), second = Luminance(b);
    return ((std::max)(first, second) + 0.05) / ((std::min)(first, second) + 0.05);
}
} // namespace

TEST_CASE(wallpaper_palette_keeps_text_readable_for_bright_dark_and_neutral_wallpapers)
{
    for (const auto seed : {0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFF00u, 0xFFFFFFu, 0x000000u, 0x808080u})
    {
        for (const bool light : {true, false})
        {
            const auto palette = MakeWallpaperPalette(seed, light);
            REQUIRE(Contrast(palette.text, palette.surface) >= 7);
            REQUIRE(Contrast(palette.text, palette.selected) >= 4.5);
            REQUIRE(Contrast(palette.secondary, palette.surface) >= 4.5);
            REQUIRE(Contrast(palette.accent, palette.surface) >= 3);
        }
    }
}

TEST_CASE(emoji_separated_pinyin_matches_thinking_and_follows_its_word)
{
    const auto compact = metasequoia::local_modes::query_emoji("sikao", SchemeType::Quanpin, 3);
    const auto separated = metasequoia::local_modes::query_emoji("si'kao", SchemeType::Quanpin, 3);
    REQUIRE(!compact.candidates.empty());
    REQUIRE(!separated.candidates.empty());
    REQUIRE_EQ(separated.candidates.front().word, std::string("🤔"));
    REQUIRE_EQ(compact.candidates.front().word, separated.candidates.front().word);
    REQUIRE_EQ(separated.candidates.front().pinyin, std::string("si'kao"));
    std::vector<WordItem> items = {WordItem("si'kao", "思考", 100), WordItem("si", "四", 90),
                                   separated.candidates.front()};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[0].word, std::string("思考"));
    REQUIRE_EQ(items[1].word, std::string("🤔"));
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[1].word, std::string("🤔"));
    REQUIRE(metasequoia::local_modes::query_emoji("'''", SchemeType::Quanpin, 3).candidates.empty());
}
