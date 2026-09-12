// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_STAKING_H
#define BITCOIN_WALLET_STAKING_H

#include <interfaces/staking.h>
#include <primitives/block.h>
#include <script/standard.h>
#include <sync.h>
#include <uint256.h>
#include <wallet/coinselection.h>

#include <memory>
#include <set>
#include <stdint.h>
#include <string>

class CWallet;
extern RecursiveMutex cs_main;

//! Wallet-side implementation of the node's staking interfaces.
//!
//! Everything here lives in libbitcoin_wallet, so a --disable-wallet build
//! simply has no staking implementation and libbitcoin_server never references
//! a wallet symbol. See src/interfaces/staking.h for the contract.

//! Sum the coin-age weight of the coins this wallet would stake, on demand:
//! scans the wallet under cs_wallet and reads each coin from disk, so on a
//! large wallet this takes seconds. While a staking thread runs, prefer the
//! weight it publishes through CWallet::GetPublishedStakeWeight.
bool GetStakeWeight(const CWallet* pwallet, uint64_t& nAverageWeight, uint64_t& nTotalWeight, const Consensus::Params& consensusParams);

//! Stake weight of the coins one CreateCoinStake pass examined, summed the
//! way GetStakeWeight sums it. Complete when the pass ran over every coin,
//! whether or not it found a kernel; incomplete only when the pass failed
//! before it could, in which case the previously published value should
//! stand. A pass that found nothing to stake is complete with zero weight.
struct StakeWeightSummary {
    uint64_t average{0};
    uint64_t total{0};
    bool complete{false};
};

//! The coins one wallet may stake, collected under cs_wallet and reused by
//! the staking thread across passes until the wallet's view of the chain
//! moves. The set is ordered by outpoint, which is the order the kernel
//! search has always run in.
struct StakeCandidates {
    std::set<CInputCoin> coins;
    //! Value of the coins and the reserve at collection; a coinstake may not
    //! spend more than their difference.
    CAmount nAvailable{0};
    CAmount nReserveBalance{0};
    //! The wallet's last processed block when collected. The staking thread
    //! collects again when this moves.
    uint256 hashLastBlock;
    //! False until a collection has run, and false again if the reserve
    //! balance setting could not be parsed.
    bool collected{false};
};

//! Collect the wallet's stakeable coins. Takes cs_wallet for the duration,
//! which on a large wallet is seconds, so the staking thread calls this once
//! per block rather than once per pass. Returns false when there is nothing
//! to stake or the reserve balance setting is invalid; candidates.collected
//! tells the two apart.
bool CollectStakeCandidates(const CWallet* pwallet, StakeCandidates& candidates);

//! A kernel SearchStakeKernel found, carrying what BuildCoinStake needs to
//! place it as the coinstake's first input.
struct StakeKernel {
    COutPoint outpoint;
    CTxOut txout;
    CBlockHeader header;      //!< block the coin was created in
    CTransactionRef txPrev;   //!< transaction that created it
    uint32_t nTime{0};        //!< coinstake time at which the kernel met the target
    CScript scriptPubKeyOut;  //!< output script: the coin's own, or its pubkey for a keyhash coin
    TxoutType type{TxoutType::NONSTANDARD};
};

//! Search the candidates for a kernel at nTimeTx and up to nSearchInterval
//! seconds before it. Takes no wallet lock: each coin's source is read
//! through the transaction index, and cs_main is held only around the chain
//! reads for one coin at a time, so the search runs while the GUI and RPC
//! use the wallet and the node validates blocks. Reports the stake weight of
//! the coins it examined through weight, if given.
bool SearchStakeKernel(const CWallet* pwallet, CChainState* chainstate, const StakeCandidates& candidates, unsigned int nBits, int64_t nSearchInterval, uint32_t nTimeTx, const Consensus::Params& consensusParams, StakeKernel& kernel, StakeWeightSummary* weight = nullptr);

//! Build the coinstake around a found kernel. The search ran without the
//! locks and possibly against an older tip, so first check the kernel is
//! still unspent, mature and meets the target at the current tip; then add
//! the combine inputs, the dev output, the reward and the signatures.
bool BuildCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, const StakeCandidates& candidates, const StakeKernel& kernel, CMutableTransaction& txNew, const Consensus::Params& consensusParams) EXCLUSIVE_LOCKS_REQUIRED(::cs_main, pwallet->cs_wallet);

//! Collect, search and build in one call, under cs_main and cs_wallet, for
//! callers that build a block on demand such as the generate RPCs. The
//! staking thread uses the three steps separately, through
//! interfaces::StakingWallet, so that its search holds neither lock.
//! Reports the weight of the coins it examined through weight, if given.
bool CreateCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, int64_t nSearchInterval, CMutableTransaction& txNew, const Consensus::Params& consensusParams, StakeWeightSummary* weight = nullptr);

//! Recompute a coinstake's reward outputs to include the block's transaction fees
//! and re-sign it. CreateCoinStake builds the coinstake before the block's fees
//! are known (fees=0); the block assembler calls this after addPackageTxs so the
//! coinstake actually collects the fees, split 92/8, matching the validator's
//! fee-inclusive reward in ConnectBlock. Only output amounts and signatures
//! change; the kernel/inputs/nTime are untouched.
bool FinalizeCoinStakeReward(const CWallet* pwallet, CChainState* chainstate, CMutableTransaction& txCoinStake, const CAmount& nFees, const Consensus::Params& consensusParams);

//! Sign a block with the key behind its coinstake (or coinbase) output.
//! CheckBlockSignature, the verification side, is consensus and stays in
//! src/pos/signer.h.
bool SignBlock(CBlock& block, const CWallet& keystore);

//! Wrap a loaded wallet in the node-facing staking interface.
std::unique_ptr<interfaces::StakingWallet> MakeStakingWallet(const std::shared_ptr<CWallet>& wallet);

//! Staking support backed by the process's loaded wallets.
//!
//! Registered on NodeContext::staking_support by WalletInit::Construct(). The
//! returned reference has static storage duration.
interfaces::StakingSupport& GetWalletStakingSupport();

#endif // BITCOIN_WALLET_STAKING_H
