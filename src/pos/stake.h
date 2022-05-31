// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef POS_STAKE_H
#define POS_STAKE_H

#include <consensus/params.h>
#include <wallet/wallet.h>

extern int64_t nLastCoinStakeSearchInterval;

class CChainState;

bool GetStakeWeight(const CWallet* pwallet, uint64_t& nAverageWeight, uint64_t& nTotalWeight, const Consensus::Params& consensusParams);
bool CreateCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, int64_t nSearchInterval, CMutableTransaction& txNew, const Consensus::Params& consensusParams);

#endif // POS_STAKE_H

