// Copyright (c) 2021-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/modifiercache.h>

unsigned int cacheHit, cacheMiss;
std::map<cachedModifier, uint64_t> cachedModifiers;

void cacheInit()
{
    cacheHit = 0;
    cacheMiss = 0;
    cachedModifiers.clear();
}

void cacheAdd(cachedModifier entry, uint64_t& nStakeModifier)
{
    cachedModifiers[entry] = nStakeModifier;
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

bool cacheCheck(cachedModifier entry, uint64_t& nStakeModifier)
{
    cacheDebug();
    cacheMaintain();

    if (!cachedModifiers.count(entry)) {
        cacheMiss++;
        return false;
    }

    nStakeModifier = cachedModifiers[entry];
    cacheHit++;
    return true;
}

