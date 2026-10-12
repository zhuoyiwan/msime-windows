#include "robust_sentence.h"
#include "quanpin_query.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace quanpin
{
namespace
{
constexpr size_t kMaxLetters = 48;
constexpr size_t kPhraseBeam = 24;
constexpr size_t kQueryKeys = 512;
struct Reading
{
    size_t end = 0;
    Segments parts;
    unsigned cost = 0;
    double rank = 0;
};
bool adjacent(char a, char b)
{
    static constexpr const char *neighbors[] = {"qwsz",   "vghn", "xdfv",   "serfcx", "wrsd",   "drtgvc", "ftyhbv",
                                                "gyujnb", "uojk", "huikmn", "jiolm",  "kop",    "njk",    "bhjm",
                                                "ipkl",   "ol",   "wa",     "etdf",   "awdexz", "ryfg",   "yihj",
                                                "cfgb",   "qesa", "zsdc",   "tugh",   "asx"};
    return a >= 'a' && a <= 'z' && std::string_view(neighbors[a - 'a']).find(b) != std::string_view::npos;
}
unsigned distance(std::string_view raw, std::string_view target, unsigned types)
{
    std::array<std::array<unsigned, 8>, 9> costs{};
    for (auto &row : costs)
        row.fill(100);
    costs[0][0] = 0;
    for (size_t i = 0; i <= raw.size(); ++i)
    {
        for (size_t j = 0; j <= target.size(); ++j)
        {
            const auto here = costs[i][j];
            if (i < raw.size() && (types & kAutocorrectInsertion))
                costs[i + 1][j] = (std::min)(costs[i + 1][j], here + 4);
            if (j < target.size() && (types & kAutocorrectDeletion))
                costs[i][j + 1] = (std::min)(costs[i][j + 1], here + 4);
            if (i < raw.size() && j < target.size())
            {
                const unsigned cost = raw[i] == target[j]               ? 0
                                      : !(types & kAutocorrectNeighbor) ? 100
                                      : adjacent(raw[i], target[j])     ? 4
                                                                        : 7;
                costs[i + 1][j + 1] = (std::min)(costs[i + 1][j + 1], here + cost);
            }
            if (i + 1 < raw.size() && j + 1 < target.size() && raw[i] == target[j + 1] && raw[i + 1] == target[j] &&
                (types & kAutocorrectTransposition))
                costs[i + 2][j + 2] = (std::min)(costs[i + 2][j + 2], here + 3);
        }
    }
    return costs[raw.size()][target.size()];
}
void rank(std::vector<Reading> &items)
{
    std::stable_sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        if (a.parts != b.parts)
            return a.parts < b.parts;
        return a.end < b.end;
    });
}
} // namespace

std::vector<LatticePath> decode_robust_sentence(const std::string &raw, unsigned types, const WordLatticeLookup &lookup,
                                                const WordLatticeOptions &options)
{
    if (raw.size() < 8 || raw.size() > kMaxLetters || !types || !lookup ||
        !std::all_of(raw.begin(), raw.end(), [](char c) { return c >= 'a' && c <= 'z'; }))
        return {};
    std::unordered_map<std::string, std::vector<LatticeLexeme>> rows;
    std::vector<std::string> syllables;
    double unigram_z = 0;
    for (const auto &key : intact_pinyin_list())
    {
        const auto values = lookup({key});
        if (!values.empty())
        {
            for (const auto &row : values)
                unigram_z += static_cast<double>((std::max)(int64_t{1}, row.weight));
            syllables.push_back(key);
            rows.emplace(key, values);
        }
    }
    std::vector<std::vector<Reading>> readings(raw.size());
    for (size_t start = 0; start < raw.size(); ++start)
    {
        auto &choices = readings[start];
        for (size_t end = start + 1; end <= (std::min)(raw.size(), start + 7); ++end)
        {
            const auto piece = std::string_view(raw).substr(start, end - start);
            for (const auto &key : syllables)
            {
                auto cost = distance(piece, key, types);
                if (end == raw.size() && key.compare(0, piece.size(), piece) == 0)
                    cost = (std::min)(cost, 2u);
                if (cost > 8 || (piece.size() == 1 && cost && end != raw.size()))
                    continue;
                const auto weight = (std::max)(int64_t{1}, rows[key].front().weight);
                choices.push_back({end, {key}, cost, 2.0 * cost - 0.35 * std::log(static_cast<double>(weight))});
            }
        }
        rank(choices);
        // 先保留每种消费长度的最佳编辑边，避免同读音的短边挤掉漏字/多字的完整解释。
        std::unordered_set<size_t> selected;
        std::unordered_set<size_t> ends;
        for (size_t i = 0; i < choices.size(); ++i)
            if (choices[i].cost && ends.insert(choices[i].end).second)
                selected.insert(i);
        for (size_t i = 0; i < choices.size() && selected.size() < 16; ++i)
            if (choices[i].cost)
                selected.insert(i);
        size_t at = 0;
        choices.erase(std::remove_if(choices.begin(), choices.end(),
                                     [&](const auto &choice) {
                                         const bool keep = !choice.cost || selected.count(at) != 0;
                                         ++at;
                                         return !keep;
                                     }),
                      choices.end());
    }
    std::vector<LetterLexeme> edges;
    const size_t keys_per_start = kQueryKeys / raw.size();
    for (size_t start = 0; start < raw.size(); ++start)
    {
        auto frontier = readings[start];
        size_t start_keys = 0;
        for (size_t depth = 1; depth <= 3; ++depth)
        {
            rank(frontier);
            if (frontier.size() > kPhraseBeam)
                frontier.resize(kPhraseBeam);
            std::vector<Reading> next;
            for (const auto &span : frontier)
            {
                const auto key = join_segments(span.parts);
                auto found = rows.find(key);
                if (found == rows.end() && start_keys < keys_per_start)
                {
                    ++start_keys;
                    found = rows.emplace(key, lookup(span.parts)).first;
                }
                if (found != rows.end())
                    for (const auto &row : found->second)
                        edges.push_back({start, span.end, row, 2.0 * span.cost});
                if (depth < 3 && span.end < raw.size())
                {
                    for (const auto &tail : readings[span.end])
                    {
                        if (span.cost + tail.cost > 12)
                            continue;
                        auto parts = span.parts;
                        parts.push_back(tail.parts.front());
                        next.push_back({tail.end, std::move(parts), span.cost + tail.cost, span.rank + tail.rank});
                    }
                }
            }
            frontier = std::move(next);
        }
    }
    auto settings = options;
    settings.beam = 24;
    settings.nbest = 6;
    settings.unigram_z = (std::max)(settings.unigram_z, unigram_z);
    return decode_letter_lattice(raw.size(), edges, settings);
}
} // namespace quanpin
