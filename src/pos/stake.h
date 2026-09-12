// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_STAKE_H
#define BITCOIN_POS_STAKE_H

#include <consensus/params.h>
#include <primitives/block.h>
#include <script/standard.h>
#include <wallet/wallet.h>

#include <set>

class CChainState;

// logging defaults
static const bool DEFAULT_PRINTCOINSTAKE = false;

/** Stake weight of the wallet's stakeable coins, computed on demand: scans
 *  the wallet under cs_wallet and reads each coin from disk, so on a large
 *  wallet this takes seconds. While a staking thread runs, prefer the weight
 *  it publishes through CWallet::GetPublishedStakeWeight. */
bool GetStakeWeight(const CWallet* pwallet, uint64_t& nAverageWeight, uint64_t& nTotalWeight, const Consensus::Params& consensusParams);

/** Stake weight of the coins one search pass examined, summed the way
 *  GetStakeWeight sums it. Complete when the pass ran over every coin,
 *  whether or not it found a kernel; incomplete only when the pass failed
 *  before it could, in which case the previously published value should
 *  stand. A pass that found nothing to stake is complete with zero weight. */
struct StakeWeightSummary {
    uint64_t average{0};
    uint64_t total{0};
    bool complete{false};
};

/** The coins one wallet may stake, collected under cs_wallet and reused by
 *  the staking thread across passes until the wallet's view of the chain
 *  moves. The set is ordered by outpoint, which is the order the kernel
 *  search has always run in. */
struct StakeCandidates {
    std::set<CInputCoin> coins;
    //! Trusted balance and reserve at collection; a coinstake may not spend
    //! more than their difference.
    CAmount nBalance{0};
    CAmount nReserveBalance{0};
    //! The wallet's last processed block when collected. The staking thread
    //! collects again when this moves.
    uint256 hashLastBlock;
    //! False until a collection has run, and false again if the reserve
    //! balance setting could not be parsed.
    bool collected{false};
};

/** Collect the wallet's stakeable coins. Takes cs_wallet for the duration,
 *  which on a large wallet is seconds, so the staking thread calls this once
 *  per block rather than once per pass. Returns false when there is nothing
 *  to stake or the reserve balance setting is invalid; candidates.collected
 *  tells the two apart. */
bool CollectStakeCandidates(const CWallet* pwallet, StakeCandidates& candidates);

/** A kernel SearchStakeKernel found, carrying what BuildCoinStake needs to
 *  place it as the coinstake's first input. */
struct StakeKernel {
    COutPoint outpoint;
    CTxOut txout;
    CBlockHeader header;      //!< block the coin was created in
    CTransactionRef txPrev;   //!< transaction that created it
    uint32_t nTime{0};        //!< coinstake time at which the kernel met the target
    CScript scriptPubKeyOut;  //!< output script: the coin's own, or its pubkey for a keyhash coin
    TxoutType type{TxoutType::NONSTANDARD};
};

/** Search the candidates for a kernel at nTimeTx and up to nSearchInterval
 *  seconds before it. Takes no wallet lock: each coin's source is read
 *  through the transaction index and cs_main is held only around the kernel
 *  check itself, so the search runs while the GUI and RPC use the wallet.
 *  Reports the stake weight of the coins it examined through weight, if
 *  given. */
bool SearchStakeKernel(const CWallet* pwallet, CChainState* chainstate, const StakeCandidates& candidates, unsigned int nBits, int64_t nSearchInterval, uint32_t nTimeTx, const Consensus::Params& consensusParams, StakeKernel& kernel, StakeWeightSummary* weight = nullptr);

/** Build the coinstake around a found kernel. The search ran without the
 *  locks and possibly against an older tip, so first check the kernel is
 *  still unspent, mature and meets the target at the current tip; then add
 *  the combine inputs, the dev output, the reward and the signatures.
 *  Requires cs_main and cs_wallet. */
bool BuildCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, const StakeCandidates& candidates, const StakeKernel& kernel, CMutableTransaction& txNew, const Consensus::Params& consensusParams);

/** Collect, search and build in one call, under cs_main and cs_wallet, for
 *  callers that build a block on demand such as the generate RPCs. The
 *  staking thread uses the three steps separately so that its search holds
 *  neither lock. */
bool CreateCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, int64_t nSearchInterval, CMutableTransaction& txNew, const Consensus::Params& consensusParams, StakeWeightSummary* weight = nullptr);

#endif // BITCOIN_POS_STAKE_H
