// Copyright (c) 2021-2023 The Reddcoin Core developers
// Copyright (c) 2012-2021 The Peercoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/kernelrecord.h>

#include <arith_uint256.h>
#include <base58.h>
#include <chainparams.h>
#include <key_io.h>
#include <timedata.h>
#include <interfaces/wallet.h>
#include <wallet/wallet.h>

#include <math.h>

bool KernelRecord::showTransaction(bool isCoinbase, int depth)
{
    if (isCoinbase) {
        if (depth < 2)
            return false;
    } else {
        if (depth == 0)
            return false;
    }

    return true;
}

bool KernelRecord::showTransaction(bool isCoinbase, bool isCoinstake, int depth)
{
    if (isCoinbase) {
        if (depth < 2)
            return false;
    } else if (isCoinstake) {
        if (depth < 0)
            return false;
    } else {
        if (depth <= 0)
            return false;
    }

    return true;
}

std::vector<KernelRecord> KernelRecord::decomposeOutput(interfaces::Wallet& wallet, const interfaces::WalletTx &wtx)
{
    std::vector<KernelRecord> parts;
    int64_t nTime = wtx.tx->nTime;
    uint256 hash = wtx.tx->GetHash();
    std::map<std::string, std::string> mapValue = wtx.value_map;

    int numBlocks;
    interfaces::WalletTxStatus status;
    interfaces::WalletOrderForm orderForm;
    bool inMempool;
    wallet.getWalletTxDetails(hash, status, orderForm, inMempool, numBlocks);

    if (showTransaction(wtx.is_coinbase, wtx.is_coinstake, status.depth_in_main_chain)) {
        for (size_t nOut = 0; nOut < wtx.tx->vout.size(); nOut++) {
            CTxOut txOut = wtx.tx->vout[nOut];
            if (wallet.txoutIsMine(txOut)) {
                CTxDestination address;
                std::string addrStr;

                if (ExtractDestination(txOut.scriptPubKey, address)) {
                    // Sent to Bitcoin Address
                    addrStr = EncodeDestination(address);
                } else {
                    // Sent to IP, or other non-address transaction like OP_EVAL
                    addrStr = mapValue["to"];
                }
                std::vector<interfaces::WalletTxOut> coins = wallet.getCoins({COutPoint(hash, nOut)});
                bool isSpent = coins.size() >= 1 ? coins[0].is_spent : true;
                parts.push_back(KernelRecord(hash, nTime, addrStr, txOut.nValue, nOut, isSpent));
            }
        }
    }

    return parts;
}

std::string KernelRecord::getTxID()
{
    return hash.ToString() + strprintf("-%03d", idx);
}

int64_t KernelRecord::getAge() const
{
    return (GetAdjustedTime() - nTime) / 3600;
}

int64_t KernelRecord::getCoinAge() const
{
    arith_uint256 bnCoinDay = arith_uint256(nValue) * getCoinAgeWeight() / COIN / (24 * 60 * 60);
    int64_t nCoinAge = ArithToUint256(bnCoinDay).GetUint64(0);
    return std::max(nCoinAge, (int64_t)0);
}

int64_t KernelRecord::getCoinAgeWeight(int nTimeOffset) const
{
    const Consensus::Params& params = Params().GetConsensus();
    int64_t nSeconds = std::max((int64_t)0, GetAdjustedTime() - nTime - params.nStakeMinAge + nTimeOffset);
    double days = double(nSeconds) / (24 * 60 * 60);
    double weight = 0;

    if (days <= 7) {
        weight = -0.00408163 * pow(days, 3) + 0.05714286 * pow(days, 2) + days;
    } else {
        weight = 8.4 * log(days) - 7.94564525;
    }

    return std::min((int64_t)(weight * 24 * 60 * 60), params.nStakeMaxAge);
}

double KernelRecord::getProbToMintStake(double difficulty, int timeOffset) const
{
    double maxTarget = pow(static_cast<double>(2), 224);
    double target = maxTarget / difficulty;

    arith_uint256 bnCoinDay = arith_uint256(nValue) * getCoinAgeWeight(timeOffset) / COIN / (24 * 60 * 60);
    int64_t nCoinAge = ArithToUint256(bnCoinDay).GetUint64(0);
    uint64_t coinAge = std::max((int64_t)0, nCoinAge);
    return target * coinAge / std::pow(static_cast<double>(2), 256);
}

double KernelRecord::getProbToMintWithinNMinutes(double difficulty, int minutes)
{
    if(difficulty != prevDifficulty || minutes != prevMinutes)
    {
        double prob = 1;
        double p;
        int d = minutes / (60 * 24); // Number of full days
        int m = minutes % (60 * 24); // Number of minutes in the last day
        int i, timeOffset;

        // Probabilities for the first d days
        for(i = 0; i < d; i++)
        {
            timeOffset = i * 86400;
            p = pow(1 - getProbToMintStake(difficulty, timeOffset), 86400);
            prob *= p;
        }

        // Probability for the m minutes of the last day
        timeOffset = d * 86400;
        p = pow(1 - getProbToMintStake(difficulty, timeOffset), 60 * m);
        prob *= p;

        prob = 1 - prob;
        prevProbability = prob;
        prevDifficulty = difficulty;
        prevMinutes = minutes;
    }
    return prevProbability;
}
