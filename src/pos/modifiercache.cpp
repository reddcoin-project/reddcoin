// Copyright (c) 2021-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/modifiercache.h>

unsigned int cacheHit, cacheMiss;
std::map<uint256, CachedStakeModifier> cachedModifiers;

void cacheInit()
{
    cacheHit = 0;
    cacheMiss = 0;
    cachedModifiers.clear();
}

void cacheAdd(const uint256& hashBlockFrom, const CachedStakeModifier& entry)
{
    cachedModifiers[hashBlockFrom] = entry;
}

void cacheDebug()
{
    if (gArgs.GetBoolArg("-debug", false)) {
        LogPrintf("hits %d / miss %d / total %d\n", cacheHit, cacheMiss, cacheHit + cacheMiss);
    }
}

void cacheMaintain()
{
    if (cacheHit + cacheMiss >
        DEFAULT_FLUSH_MODIFIER_CACHE) cacheInit();
}

bool cacheCheck(const uint256& hashBlockFrom, CachedStakeModifier& entry)
{
    cacheDebug();
    cacheMaintain();

    const auto it = cachedModifiers.find(hashBlockFrom);
    if (it == cachedModifiers.end()) {
        cacheMiss++;
        return false;
    }

    entry = it->second;
    cacheHit++;
    return true;
}

