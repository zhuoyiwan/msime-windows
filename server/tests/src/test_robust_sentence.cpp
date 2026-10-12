#include "tests/includes/test_framework.h"
#include "engine/quanpin/quanpin_dictionary.h"
#include "engine/quanpin/quanpin_utils.h"
#include <algorithm>
#include <chrono>
#include <iostream>

TEST_CASE(RobustSentenceShippedDictionaryCorrections)
{
    QuanpinDictionary dictionary;
    SentenceAssociationOptions options;
    options.word_lattice = true;
    options.google = false;
    dictionary.set_sentence_association(options);
    const unsigned all = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor |
                         quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
    for (const auto &raw : {"woxiangqushangk", "woxiangqusahngk", "woxiangqushabgk", "woxiangqushanngk",
                            "woxiangqushngk", "wixiabgqusahngk"})
    {
        const auto start = std::chrono::steady_clock::now();
        const auto rows = dictionary.query(raw, "", all);
        std::cout << raw << " ";
        for (size_t i = 0; i < (std::min)(rows.size(), size_t{6}); ++i)
            std::cout << rows[i].word << " | ";
        std::cout
            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()
            << "ms\n";
        REQUIRE(std::any_of(rows.begin(), rows.begin() + (std::min)(rows.size(), size_t{6}),
                            [](const auto &row) { return row.word == "我想去上课"; }));
        const auto selected =
            std::find_if(rows.begin(), rows.end(), [](const auto &row) { return row.word == "我想去上课"; });
        std::string consumed = selected->pinyin;
        consumed.erase(std::remove(consumed.begin(), consumed.end(), '\''), consumed.end());
        REQUIRE(consumed == raw);
    }
    const std::string raw = "wixbwozhdiwnxbyak";
    const auto rows = dictionary.query(raw, "", all);
    size_t complete = 0;
    for (const auto &row : rows)
        if (row.pinyin == raw && row.sentence_association && quanpin::is_complete_pinyin_input(row.canonical_pinyin))
            ++complete;
    REQUIRE(complete >= 2);
}
