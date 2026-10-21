// Copyright (c) 2021-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_MODIFIERCACHE_H
#define BITCOIN_POS_MODIFIERCACHE_H

#include <uint256.h>
#include <util/system.h>

#include <map>

/**
 * The kernel stake modifier chosen for a coin's block, and the block it was
 * taken from. The modifier depends on the chain after the coin's block, so a
 * cached one is only good for a chain that has that block as an ancestor.
 */
struct CachedStakeModifier {
    uint64_t nStakeModifier{0};
    uint256 hashBlock;
    int nHeight{0};
    int64_t nTime{0};
};

/** Flush the modifiercache every 250,000 blocks (approx 5mb memory) */
static const unsigned int DEFAULT_FLUSH_MODIFIER_CACHE = 250000;

void cacheInit();
void cacheAdd(const uint256& hashBlockFrom, const CachedStakeModifier& entry);
bool cacheCheck(const uint256& hashBlockFrom, CachedStakeModifier& entry);

#endif // BITCOIN_POS_MODIFIERCACHE_H
