#include "../contracts/assets/assets.h"
#include "emoji_query.h"
#include "local_database.h"

#include "../core/data_path.h"
#include "../shuangpin/shuangpin_query.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace metasequoia::local_modes
{
namespace
{
struct StatementCloser
{
    void operator()(sqlite3_stmt *statement) const
    {
        if (statement != nullptr)
        {
            sqlite3_finalize(statement);
        }
    }
};

using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

bool valid_code(const std::string &code)
{
    return !code.empty() && std::all_of(code.begin(), code.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '\'';
    });
}

std::string lower_ascii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

LocalQueryResult query_failure(const char *diagnostic)
{
    return {{}, std::string(diagnostic)};
}
} // namespace

LocalQueryResult query_emoji(const std::string &code, SchemeType scheme, int limit, const ShuangpinProfile &profile)
{
    return query_emoji(code, scheme, data_file_path(metasequoia::assets::other_dictionary), limit, profile);
}

LocalQueryResult query_emoji(const std::string &code, SchemeType scheme, const std::filesystem::path &database_path,
                             int limit, const ShuangpinProfile &profile)
{
    if (!valid_code(code) || limit <= 0)
    {
        return {};
    }
    if (database_path.empty())
    {
        return query_failure("Emoji database is unavailable.");
    }

    const std::string lower = lower_ascii(code);
    const auto compact = [](std::string text) {
        text.erase(std::remove(text.begin(), text.end(), '\''), text.end());
        return text;
    };
    std::vector<std::string> prefixes{lower};
    if (compact(lower).empty())
        return {};
    if (scheme == SchemeType::Shuangpin)
    {
        const std::string quanpin = shuangpin::normalize_input(lower, profile);
        if (!quanpin.empty() && quanpin != prefixes.front())
        {
            prefixes.push_back(quanpin);
        }
    }
    const auto original_prefixes = prefixes;
    for (const auto &prefix : original_prefixes)
    {
        const auto spelling = compact(prefix);
        if (!spelling.empty() && spelling != prefix)
            prefixes.push_back(spelling);
    }

    const std::shared_ptr<sqlite3> database = open_local_database(database_path);
    if (!database)
    {
        return query_failure("Emoji database is unavailable.");
    }

    struct Entry
    {
        std::string text;
        int sort_order;
        bool exact;
        std::string keywords;
    };
    std::vector<Entry> entries;
    std::unordered_set<std::string> seen;
    constexpr const char *kSql = "SELECT p.emoji,p.sort_order,p.key=?1,COALESCE(e.keywords,'') FROM emoji_pinyin p "
                                 "LEFT JOIN emoji e ON e.emoji=p.emoji WHERE p.key>=?1 AND p.key<?2 "
                                 "ORDER BY p.key<>?1,p.sort_order LIMIT ?3";
    constexpr const char *kLegacySql = "SELECT emoji,sort_order,key=?1,'' FROM emoji_pinyin WHERE key>=?1 AND key<?2 "
                                       "ORDER BY key<>?1,sort_order LIMIT ?3";
    for (const std::string &prefix : prefixes)
    {
        // 旧目录可能有带分隔符的编码；原编码查到结果时不扩大到紧凑前缀。
        if (!entries.empty() && prefix == compact(lower) && prefix != lower && scheme == SchemeType::Quanpin)
            break;
        sqlite3_stmt *raw_statement = nullptr;
        if (sqlite3_prepare_v2(database.get(), kSql, -1, &raw_statement, nullptr) != SQLITE_OK &&
            sqlite3_prepare_v2(database.get(), kLegacySql, -1, &raw_statement, nullptr) != SQLITE_OK)
        {
            return query_failure("Emoji database could not be queried.");
        }
        Statement statement(raw_statement);
        std::string upper_bound = prefix;
        upper_bound.push_back(static_cast<char>(0x7f));
        if (sqlite3_bind_text(statement.get(), 1, prefix.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(statement.get(), 2, upper_bound.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_int(statement.get(), 3, limit) != SQLITE_OK)
        {
            return query_failure("Emoji database could not be queried.");
        }

        int step_result = SQLITE_ROW;
        while ((step_result = sqlite3_step(statement.get())) == SQLITE_ROW)
        {
            const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 0));
            if (text != nullptr && seen.insert(text).second)
            {
                const auto *keywords = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 3));
                entries.push_back({text, sqlite3_column_int(statement.get(), 1),
                                   sqlite3_column_int(statement.get(), 2) != 0, keywords ? keywords : ""});
            }
        }
        if (step_result != SQLITE_DONE)
        {
            return query_failure("Emoji database could not be queried.");
        }
    }

    std::stable_sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
        return left.exact != right.exact ? left.exact : left.sort_order < right.sort_order;
    });
    const std::size_t count = std::min(static_cast<std::size_t>(limit), entries.size());
    LocalQueryResult result;
    result.candidates.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        result.candidates.emplace_back(lower, entries[index].text, static_cast<std::int64_t>(count - index),
                                       CandidateSource::Emoji);
        result.candidates.back().expressive_keywords = entries[index].keywords;
    }
    return result;
}
} // namespace metasequoia::local_modes
