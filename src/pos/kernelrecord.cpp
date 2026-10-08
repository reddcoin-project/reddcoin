// Copyright (c) 2021-2023 The Reddcoin Core developers
// Copyright (c) 2012-2021 The Peercoin Core developers
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

KernelRecord::KernelRecord(const interfaces::WalletMintingOutput& output):
    hash(output.outpoint.hash), nTime(output.tx_time), address(""), nValue(output.txout.nValue),
    idx(output.outpoint.n), spent(false), prevMinutes(0), prevDifficulty(0), prevProbability(0)
{
    CTxDestination destination;
    if (ExtractDestination(output.txout.scriptPubKey, destination)) {
        address = EncodeDestination(destination);
    }
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
