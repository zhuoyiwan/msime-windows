#pragma once
#include "word_lattice.h"

namespace quanpin
{
std::vector<LatticePath> decode_robust_sentence(const std::string &raw, unsigned types, const WordLatticeLookup &lookup,
                                                const WordLatticeOptions &options);
}
