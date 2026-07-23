// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_STAKE_H
#define BITCOIN_POS_STAKE_H

#include <interfaces/staking.h>

#include <stdint.h>
#include <vector>

// logging defaults
static const bool DEFAULT_PRINTCOINSTAKE = false;

// Sum the coin-age weight of a set of stakeable coins.
//
// Chain-side only: it reads each coin's originating block header through the
// transaction index, so it belongs to libbitcoin_server and takes the
// wallet-free interfaces::StakeCoin rather than CInputCoin. The wallet-side
// staking operations that used to live here (CreateCoinStake,
// FinalizeCoinStakeReward, GetStakeWeight over a wallet) are now declared in
// src/wallet/staking.h.
bool GetStakeWeight(const std::vector<interfaces::StakeCoin>& coins, uint64_t& nAverageWeight, uint64_t& nTotalWeight);

#endif // BITCOIN_POS_STAKE_H
