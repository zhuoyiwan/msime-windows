#pragma once

#include "word_item.h"
#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace metasequoia
{
// 只锚定到已有词，避免把「思考」的表情放到无关的同音词后面；无关键词的旧词库仍可使用。
inline void anchor_emoji_candidates(std::vector<WordItem> &items)
{
    if (std::none_of(items.begin(), items.end(), [](const WordItem &item) {
            return item.source == CandidateSource::Emoji && !item.expressive_keywords.empty();
        }))
        return;
    std::unordered_map<std::string, size_t> positions;
    for (size_t index = 0; index < items.size(); ++index)
        if (items[index].source != CandidateSource::Emoji)
            positions.emplace(items[index].word, index);
    std::vector<std::vector<WordItem>> anchored(items.size());
    std::vector<bool> moved(items.size(), false);
    for (size_t index = 0; index < items.size(); ++index)
    {
        auto &item = items[index];
        if (item.source != CandidateSource::Emoji)
            continue;
        size_t anchor = items.size();
        std::istringstream keywords(item.expressive_keywords);
        std::string keyword;
        while (keywords >> keyword)
        {
            const auto match = positions.find(keyword);
            if (match != positions.end())
                anchor = (std::min)(anchor, match->second);
        }
        if (anchor < items.size())
        {
            anchored[anchor].push_back(std::move(item));
            moved[index] = true;
        }
    }
    std::vector<WordItem> result;
    result.reserve(items.size());
    for (size_t index = 0; index < items.size(); ++index)
    {
        if (!moved[index])
            result.push_back(std::move(items[index]));
        for (auto &item : anchored[index])
            result.push_back(std::move(item));
    }
    items = std::move(result);
}
} // namespace metasequoia
