// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_SIGNER_H
#define BITCOIN_POS_SIGNER_H

#include <primitives/block.h>
#include <primitives/transaction.h>

// Block signature verification is consensus (see ContextualCheckBlock) and is
// wallet-free. Block *signing* needs wallet keys and lives in
// src/wallet/staking.h.
bool CheckBlockSignature(const CBlock& block);

#endif // BITCOIN_POS_SIGNER_H
