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

bool GetStakeWeight(const CWallet* pwallet, uint64_t& nAverageWeight, uint64_t& nTotalWeight, const Consensus::Params& consensusParams);
bool GetStakeWeight(std::set<CInputCoin>& setCoins, uint64_t& nAverageWeight, uint64_t& nTotalWeight);
bool CreateCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, int64_t nSearchInterval, CMutableTransaction& txNew, const Consensus::Params& consensusParams);

// Recompute a coinstake's reward outputs to include the block's transaction fees
// and re-sign it. CreateCoinStake builds the coinstake before the block's fees
// are known (fees=0); the block assembler calls this after addPackageTxs so the
// coinstake actually collects the fees, split 92/8, matching the validator's
// fee-inclusive reward in ConnectBlock. Only output amounts and signatures
// change; the kernel/inputs/nTime are untouched.
bool FinalizeCoinStakeReward(const CWallet* pwallet, CChainState* chainstate, CMutableTransaction& txCoinStake, const CAmount& nFees, const Consensus::Params& consensusParams);

#endif // BITCOIN_POS_STAKE_H

