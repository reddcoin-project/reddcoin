// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_STAKE_H
#define BITCOIN_POS_STAKE_H

// logging defaults
static const bool DEFAULT_PRINTCOINSTAKE = false;

// The staking operations that used to be declared here (CreateCoinStake,
// FinalizeCoinStakeReward, GetStakeWeight over a wallet) live in
// src/wallet/staking.h. The chain-side weight sum over a coin set that
// remained here had one caller, the GUI, which now reads the weight the
// staking thread publishes on the wallet instead of recomputing it.

#endif // BITCOIN_POS_STAKE_H
