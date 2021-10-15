// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/system.h>

#include <math.h>

unsigned int static KimotoGravityWell(const CBlockIndex* pindexLast, const CBlockHeader* pblock, uint64_t TargetBlocksSpacingSeconds, uint64_t PastBlocksMin, uint64_t PastBlocksMax)
{
    const Consensus::Params params = Params().GetConsensus();

    const CBlockIndex *BlockLastSolved = pindexLast;
    const CBlockIndex *BlockReading = pindexLast;
    uint64_t PastBlocksMass = 0;
    int64_t PastRateActualSeconds = 0;
    int64_t PastRateTargetSeconds = 0;
    double PastRateAdjustmentRatio = double(1);
    arith_uint256 PastDifficultyAverage;
    arith_uint256 PastDifficultyAveragePrev;
    double EventHorizonDeviation;
    double EventHorizonDeviationFast;
    double EventHorizonDeviationSlow;

    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    const arith_uint256 bnPosLimit = UintToArith256(params.posLimit);
    const arith_uint256 bnPosReset = UintToArith256(params.posReset);

    bool fProofOfStake = false;
    if (pindexLast && pindexLast->nHeight >= params.nLastPowHeight)
        fProofOfStake = true;

    if (BlockLastSolved == nullptr || BlockLastSolved->nHeight == 0 || (uint64_t)BlockLastSolved->nHeight < PastBlocksMin) {
        return bnPowLimit.GetCompact();
    } else if (fProofOfStake && (uint64_t)(BlockLastSolved->nHeight - params.nLastPowHeight) < PastBlocksMin) {
        return bnPosReset.GetCompact();
    }

    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > (fProofOfStake ? params.nLastPowHeight : 0); i++) {
        if (PastBlocksMax > 0 && i > PastBlocksMax)
            break;

        PastBlocksMass++;

        PastDifficultyAverage.SetCompact(BlockReading->nBits);
        if (i > 1) {
            // handle negative arith_uint256
            if(PastDifficultyAverage >= PastDifficultyAveragePrev)
                PastDifficultyAverage = ((PastDifficultyAverage - PastDifficultyAveragePrev) / i) + PastDifficultyAveragePrev;
            else
                PastDifficultyAverage = PastDifficultyAveragePrev - ((PastDifficultyAveragePrev - PastDifficultyAverage) / i);
        }
        PastDifficultyAveragePrev = PastDifficultyAverage;

        PastRateActualSeconds = BlockLastSolved->GetBlockTime() - BlockReading->GetBlockTime();
        PastRateTargetSeconds = TargetBlocksSpacingSeconds * PastBlocksMass;
        PastRateAdjustmentRatio = double(1);

        if (PastRateActualSeconds < 0)
            PastRateActualSeconds = 0;

        if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0)
            PastRateAdjustmentRatio = double(PastRateTargetSeconds) / double(PastRateActualSeconds);

        EventHorizonDeviation = 1 + (0.7084 * pow((double(PastBlocksMass) / double(144)), -1.228));
        EventHorizonDeviationFast = EventHorizonDeviation;
        EventHorizonDeviationSlow = 1 / EventHorizonDeviation;

        if (PastBlocksMass >= PastBlocksMin) {
            if ((PastRateAdjustmentRatio <= EventHorizonDeviationSlow) || (PastRateAdjustmentRatio >= EventHorizonDeviationFast)) {
                assert(BlockReading);
                break;
            }
        }

        if (BlockReading->pprev == nullptr) {
            assert(BlockReading);
            break;
        }

        BlockReading = BlockReading->pprev;
    }

    arith_uint256 bnNew(PastDifficultyAverage);

    if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
        bnNew *= PastRateActualSeconds;
        bnNew /= PastRateTargetSeconds;
    }

    if (!fProofOfStake && bnNew > bnPowLimit) {
        bnNew = bnPowLimit;
    } else if (fProofOfStake && bnNew > bnPosLimit) {
        bnNew = bnPosLimit;
    }

    return bnNew.GetCompact();
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params)
{
    static const int64_t BlocksTargetSpacing = 1 * 60; // 1 Minute
    unsigned int TimeDaySeconds = 60 * 60 * 24;

    int64_t PastSecondsMin = TimeDaySeconds * 0.25;
    int64_t PastSecondsMax = TimeDaySeconds * 7;

    if (pindexLast->nHeight < 6000) {
        PastSecondsMin = TimeDaySeconds * 0.01;
        PastSecondsMax = TimeDaySeconds * 0.14;
    }

    uint64_t PastBlocksMin = PastSecondsMin / BlocksTargetSpacing;
    uint64_t PastBlocksMax = PastSecondsMax / BlocksTargetSpacing;

    // LogPrintf("%s : Height = %s (%s) PastBlocksMin = %s, PastBlocksMax = %s \n", __func__, pindexLast->nHeight, pindexLast->GetBlockHash().ToString(), PastBlocksMin, PastBlocksMax);

    return KimotoGravityWell(pindexLast, pblock, BlocksTargetSpacing, PastBlocksMin, PastBlocksMax);
}

bool CheckProofOfWork(arith_uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Check range
    if (bnTarget <= 0 || bnTarget > UintToArith256(params.powLimit))
        return error("CheckProofOfWork() : nBits below minimum work");

    // Check proof of work matches claimed amount
    if (hash > bnTarget)
        return error("CheckProofOfWork() : hash doesn't match nBits");

    return true;
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    return CheckProofOfWork(UintToArith256(hash), nBits, params);
}

arith_uint256 GetBlockProof(const CBlockIndex& block)
{
    arith_uint256 bnTarget;
    bool fNegative;
    bool fOverflow;
    bnTarget.SetCompact(block.nBits, &fNegative, &fOverflow);
    if (fNegative || fOverflow || bnTarget == 0)
        return 0;
    // We need to compute 2**256 / (bnTarget+1), but we can't represent 2**256
    // as it's too large for an arith_uint256. However, as 2**256 is at least as large
    // as bnTarget+1, it is equal to ((2**256 - bnTarget - 1) / (bnTarget+1)) + 1,
    // or ~bnTarget / (bnTarget+1) + 1.
    return (~bnTarget / (bnTarget + 1)) + 1;
}
