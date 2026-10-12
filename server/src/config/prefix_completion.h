#pragma once

#include "config/ime_config.h"
#include "engine/local_modes/quick_phrase_query.h"
#include <string>

inline std::wstring FormatPrefixCompletionWorkerPayload()
{
    const auto scheme = GetConfiguredActiveInputScheme();
    const auto mask = (scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin) &&
                              GetConfiguredMixedCandidatesEnabled() && GetConfiguredQuickPhraseCandidatesEnabled()
                          ? metasequoia::local_modes::quick_phrase_numeric_starts()
                          : 0;
    std::wstring payload(10, L'0');
    for (unsigned digit = 0; digit < 10; ++digit)
        if (mask & (1u << digit))
            payload[digit] = L'1';
    return payload;
}
