// Copyright (c) 2021-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_MODIFIERCACHE_H
#define BITCOIN_POS_MODIFIERCACHE_H

#include <util/system.h>

#include <map>

typedef std::pair<int, int64_t> cachedModifier;

/** Flush the modifiercache every 250,000 blocks (approx 5mb memory) */
static const unsigned int DEFAULT_FLUSH_MODIFIER_CACHE = 250000;

void cacheInit();
void cacheAdd(cachedModifier entry, uint64_t& nStakeModifier);
bool cacheCheck(cachedModifier entry, uint64_t& nStakeModifier);

#endif // BITCOIN_POS_MODIFIERCACHE_H
