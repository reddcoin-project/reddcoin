// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/stake.h>

#include <arith_uint256.h>
#include <chainparams.h>
#include <clientversion.h>
#include <index/disktxpos.h>
#include <index/txindex.h>
#include <node/blockstorage.h>
#include <pos/kernel.h>
#include <primitives/block.h>
#include <streams.h>
#include <util/system.h>
#include <util/time.h>

bool GetStakeWeight(const std::vector<interfaces::StakeCoin>& coins, uint64_t& nAverageWeight, uint64_t & nTotalWeight)
{
    CChainParams chainparams(Params());

    const Consensus::Params consensusParams = chainparams.GetConsensus();

    std::vector<CTransactionRef> vwtxPrev;

    nAverageWeight = nTotalWeight = 0;
    uint64_t nWeightCount = 0;

    for (const interfaces::StakeCoin& pcoin : coins)
    {
        CDiskTxPos postx;
        if (!g_txindex) {
            return error("GetStakeWeight() : tx index not available");
        }
        if (!g_txindex->FindTxPosition(pcoin.outpoint.hash, postx))
            continue;

        // Read block header
        CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
        CBlockHeader header;
        CTransactionRef txRef;
        try {
            file >> header;
            fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
            file >> txRef;
        } catch (std::exception &e) {
            return error("%s() : deserialize or I/O error in GetStakeWeight()", __PRETTY_FUNCTION__);
        }

        CMutableTransaction tx(*txRef);

        // Deal with transaction timestamp
        unsigned int nTimeTx = tx.nTime ? tx.nTime : header.GetBlockTime();

        int64_t nTimeWeight = GetCoinAgeWeight((int64_t)nTimeTx, (int64_t)GetTime(), consensusParams);
        arith_uint512 bnCoinDayWeight = arith_uint512(pcoin.value) * nTimeWeight / COIN / (24 * 60 * 60);

        // Weight is greater than zero
        if (nTimeWeight > 0)
        {
            nTotalWeight += bnCoinDayWeight.GetLow64();
            nWeightCount++;
        }

    }

    if (nWeightCount > 0)
    nAverageWeight = nTotalWeight / nWeightCount;

    return true;
}
