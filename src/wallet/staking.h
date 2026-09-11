// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_STAKING_H
#define BITCOIN_WALLET_STAKING_H

#include <interfaces/staking.h>

#include <memory>
#include <stdint.h>
#include <string>

class CWallet;

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

//! Search for a kernel among the wallet's coins and build the coinstake.
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
