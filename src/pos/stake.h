// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_STAKE_H
#define BITCOIN_POS_STAKE_H

#include <consensus/params.h>
#include <wallet/wallet.h>

class CChainState;

// logging defaults
static const bool DEFAULT_PRINTCOINSTAKE = false;

/** Stake weight of the wallet's stakeable coins, computed on demand: scans
 *  the wallet under cs_wallet and reads each coin from disk, so on a large
 *  wallet this takes seconds. While a staking thread runs, prefer the weight
 *  it publishes through CWallet::GetPublishedStakeWeight. */
bool GetStakeWeight(const CWallet* pwallet, uint64_t& nAverageWeight, uint64_t& nTotalWeight, const Consensus::Params& consensusParams);

/** Stake weight of the coins one CreateCoinStake pass examined, summed the
 *  way GetStakeWeight sums it. Complete when the pass ran over every coin,
 *  whether or not it found a kernel; incomplete only when the pass failed
 *  before it could, in which case the previously published value should
 *  stand. A pass that found nothing to stake is complete with zero weight. */
struct StakeWeightSummary {
    uint64_t average{0};
    uint64_t total{0};
    bool complete{false};
};

bool CreateCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, int64_t nSearchInterval, CMutableTransaction& txNew, const Consensus::Params& consensusParams, StakeWeightSummary* weight = nullptr);

#endif // BITCOIN_POS_STAKE_H

