// Copyright (c) 2014-2021 The Reddcoin Core developers
// Copyright (c) 2012-2021 The Peercoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef REDDCOIN_KERNEL_H
#define REDDCOIN_KERNEL_H

#include <primitives/transaction.h> // CTransaction(Ref)
#include <validation.h>

class BlockValidationState;
class CBlock;
class CBlockHeader;
class CBlockIndex;
class CChainState;

// MODIFIER_INTERVAL_RATIO:
// ratio of group interval length between the last group and the first group
static const int MODIFIER_INTERVAL_RATIO = 3;

// Compute the hash modifier for proof-of-stake
bool ComputeNextStakeModifier(CChainState* active_chainstate, const CBlockIndex* pindexPrev, uint64_t &nStakeModifier, bool& fGeneratedStakeModifier);

// Check whether stake kernel meets hash target
// Sets hashProofOfStake on success return
bool CheckStakeKernelHash(CChainState* active_chainstate, unsigned int nBits, const CBlockHeader& blockFrom, unsigned int nTxPrevOffset, const CTransactionRef& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, bool fPrintProofOfStake = false);

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CChainState* active_chainstate, CBlockIndex* pindexPrev, const CTransactionRef& tx, unsigned int nBits, uint256& hashProofOfStake);

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx);

// Function to retrieve the coin age of a given transaction
uint64_t GetCoinAge(CChainState* active_chainstate, const CTransaction& tx, const Consensus::Params& consensusParams);

#endif // REDDCOIN_KERNEL_H
