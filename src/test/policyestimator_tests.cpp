// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <clientversion.h>
#include <fs.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <streams.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/serfloat.h>
#include <util/time.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>

BOOST_FIXTURE_TEST_SUITE(policyestimator_tests, BasicTestingSetup)

// The buckets start at the relay fee, below which no transaction goes, and
// are spaced 5% apart up to 10000 times that; the last takes everything above.
// They are read from the file the estimator writes.
BOOST_AUTO_TEST_CASE(buckets_start_at_the_relay_fee)
{
    CBlockPolicyEstimator feeEst;
    const fs::path path{m_path_root / "fee_estimates.dat"};
    {
        CAutoFile file(fsbridge::fopen(path, "wb"), SER_DISK, CLIENT_VERSION);
        BOOST_REQUIRE(feeEst.Write(file));
    }
    CAutoFile file(fsbridge::fopen(path, "rb"), SER_DISK, CLIENT_VERSION);
    int required, wrote;
    unsigned int best_seen, first, best;
    // Doubles are written as their encoding, 64 bits each.
    std::vector<uint64_t> encoded;
    file >> required >> wrote >> best_seen >> first >> best >> encoded;
    std::vector<double> buckets;
    for (const uint64_t e : encoded) buckets.push_back(DecodeDouble(e));
    BOOST_REQUIRE(!buckets.empty());
    BOOST_CHECK_EQUAL(buckets.front(), double{DEFAULT_MIN_RELAY_TX_FEE});
    // 100000 * 1.05^188 is the last below 1e9: 189 boundaries and the last bucket.
    BOOST_CHECK_EQUAL(buckets.size(), 190U);
    BOOST_CHECK_LE(buckets[buckets.size() - 2], 1e9);
    BOOST_CHECK_GT(buckets[buckets.size() - 2] * 1.05, 1e9);
    BOOST_CHECK_EQUAL(buckets.back(), 1e99);
}

BOOST_AUTO_TEST_CASE(BlockPolicyEstimates)
{
    CBlockPolicyEstimator feeEst;
    CTxMemPool mpool(&feeEst);
    LOCK2(cs_main, mpool.cs);
    TestMemPoolEntryHelper entry;
    // ReddCoin: 100 times Bitcoin's, as the relay fee and the lowest
    // bucket are, so that every fee lands in a bucket of its own.
    CAmount basefee(200000);
    CAmount deltaFee(10000);
    std::vector<CAmount> feeV;

    // Populate vectors of increasing fees
    for (int j = 0; j < 10; j++) {
        feeV.push_back(basefee * (j+1));
    }

    // Store the hashes of transactions that have been
    // added to the mempool by their associate fee
    // txHashes[j] is populated with transactions either of
    // fee = basefee * (j+1)
    std::vector<uint256> txHashes[10];

    // Create a transaction template
    CScript garbage;
    for (unsigned int i = 0; i < 128; i++)
        garbage.push_back('X');
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptSig = garbage;
    tx.vout.resize(1);
    tx.vout[0].nValue=0LL;
    CFeeRate baseRate(basefee, GetVirtualTransactionSize(CTransaction(tx)));

    // Create a fake block
    std::vector<CTransactionRef> block;
    int blocknum = 0;

    // Loop through 200 blocks
    // At 4 fee transactions per block in each bucket, well above the 0.01 threshold
    while (blocknum < 200) {
        for (int j = 0; j < 10; j++) { // For each fee
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k; // make transaction unique
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(entry.Fee(feeV[j]).Time(GetTime()).Height(blocknum).FromTx(tx));
                txHashes[j].push_back(hash);
            }
        }
        //Create blocks where higher fee txs are included more often
        for (int h = 0; h <= blocknum%10; h++) {
            // 10/10 blocks add highest fee transactions
            // 9/10 blocks add 2nd highest and so on until ...
            // 1/10 blocks add lowest fee transactions
            while (txHashes[9-h].size()) {
                CTransactionRef ptx = mpool.get(txHashes[9-h].back());
                if (ptx)
                    block.push_back(ptx);
                txHashes[9-h].pop_back();
            }
        }
        mpool.removeForBlock(block, ++blocknum);
        block.clear();
        // Check after just a few txs that combining buckets works as expected
        if (blocknum == 3) {
            // At this point we should need to combine 3 buckets to get enough data points
            // So estimateFee(1) should fail and estimateFee(2) should return somewhere around
            // 9*baserate.  estimateFee(2) %'s are 100,100,90 = average 97%
            BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
            BOOST_CHECK(feeEst.estimateFee(2).GetFeePerK() < 9*baseRate.GetFeePerK() + deltaFee);
            BOOST_CHECK(feeEst.estimateFee(2).GetFeePerK() > 9*baseRate.GetFeePerK() - deltaFee);
        }
    }

    std::vector<CAmount> origFeeEst;
    // Highest feerate is 10*baseRate and gets in all blocks,
    // second highest feerate is 9*baseRate and gets in 9/10 blocks = 90%,
    // third highest feerate is 8*base rate, and gets in 8/10 blocks = 80%,
    // so estimateFee(1) would return 10*baseRate but is hardcoded to return failure
    // Second highest feerate has 100% chance of being included by 2 blocks,
    // so estimateFee(2) should return 9*baseRate etc...
    for (int i = 1; i < 10;i++) {
        origFeeEst.push_back(feeEst.estimateFee(i).GetFeePerK());
        if (i > 2) { // Fee estimates should be monotonically decreasing
            BOOST_CHECK(origFeeEst[i-1] <= origFeeEst[i-2]);
        }
        int mult = 11-i;
        if (i % 2 == 0) { //At scale 2, test logic is only correct for even targets
            BOOST_CHECK(origFeeEst[i-1] < mult*baseRate.GetFeePerK() + deltaFee);
            BOOST_CHECK(origFeeEst[i-1] > mult*baseRate.GetFeePerK() - deltaFee);
        }
    }
    // Fill out rest of the original estimates
    for (int i = 10; i <= 48; i++) {
        origFeeEst.push_back(feeEst.estimateFee(i).GetFeePerK());
    }

    // Mine 50 more blocks with no transactions happening, estimates shouldn't change
    // We haven't decayed the moving average enough so we still have enough data points in every bucket
    while (blocknum < 250)
        mpool.removeForBlock(block, ++blocknum);

    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() < origFeeEst[i-1] + deltaFee);
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }


    // Mine 15 more blocks with lots of transactions happening and not getting mined
    // Estimates should go up
    while (blocknum < 265) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(entry.Fee(feeV[j]).Time(GetTime()).Height(blocknum).FromTx(tx));
                txHashes[j].push_back(hash);
            }
        }
        mpool.removeForBlock(block, ++blocknum);
    }

    for (int i = 1; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i) == CFeeRate(0) || feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }

    // Mine all those transactions
    // Estimates should still not be below original
    for (int j = 0; j < 10; j++) {
        while(txHashes[j].size()) {
            CTransactionRef ptx = mpool.get(txHashes[j].back());
            if (ptx)
                block.push_back(ptx);
            txHashes[j].pop_back();
        }
    }
    mpool.removeForBlock(block, 266);
    block.clear();
    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i) == CFeeRate(0) || feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }

    // Mine 4000 more blocks where everything is mined every block
    // Estimates should be below original estimates. The decays are set for
    // 60-second blocks, so the old data takes 10 times as many blocks to fade
    // as on Bitcoin, which mined 400 here.
    while (blocknum < 4265) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                uint256 hash = tx.GetHash();
                mpool.addUnchecked(entry.Fee(feeV[j]).Time(GetTime()).Height(blocknum).FromTx(tx));
                CTransactionRef ptx = mpool.get(hash);
                if (ptx)
                    block.push_back(ptx);

            }
        }
        mpool.removeForBlock(block, ++blocknum);
        block.clear();
    }
    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 9; i++) { // At 9, the original estimate was already at the bottom (b/c scale = 2)
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() < origFeeEst[i-1] - deltaFee);
    }
}

// The estimate for a chain where one transaction comes every `every` blocks,
// each paying the same fee and mined in the next block.
static CFeeRate EstimateAtRate(int every, int blocks)
{
    CBlockPolicyEstimator feeEst;
    CTxMemPool mpool(&feeEst);
    LOCK2(cs_main, mpool.cs);
    TestMemPoolEntryHelper entry;
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.resize(1);
    for (int blocknum = 0; blocknum < blocks; blocknum++) {
        std::vector<CTransactionRef> block;
        if (blocknum % every == 0) {
            tx.vin[0].prevout.n = blocknum;
            mpool.addUnchecked(entry.Fee(200000).Time(GetTime()).Height(blocknum).FromTx(tx));
            block.push_back(mpool.get(tx.GetHash()));
        }
        mpool.removeForBlock(block, blocknum + 1);
    }
    return feeEst.estimateFee(2);
}

// The estimator needs on average SUFFICIENT_FEETXS transactions a block, 0.01,
// one every 100 minutes as on Bitcoin. Mainnet carries about 0.015 a block
// (2026-11), which Bitcoin's 0.1 a block never gave an estimate for.
BOOST_AUTO_TEST_CASE(estimate_at_reddcoin_volume)
{
    // One transaction every 50 blocks, 0.02 a block: an estimate.
    BOOST_CHECK(EstimateAtRate(50, 6000) != CFeeRate(0));
    // One every 200 blocks, 0.005 a block: too few.
    BOOST_CHECK(EstimateAtRate(200, 6000) == CFeeRate(0));
}

// fee_estimates.dat says which version it needs to be read. A file written
// before the estimator's parameters were set for Reddcoin needs less than
// FEE_ESTIMATES_VERSION_REQUIRED and is discarded, since reading it would
// keep the old buckets, decay and scale; one this build writes is read back;
// one that needs a later version is refused.
BOOST_AUTO_TEST_CASE(fee_estimates_file_version)
{
    CBlockPolicyEstimator feeEst;
    {
        CTxMemPool mpool(&feeEst);
        LOCK2(cs_main, mpool.cs);
        TestMemPoolEntryHelper entry;
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vout.resize(1);
        // Ten transactions a block at each of ten fees, every one mined in
        // the next block: enough for an estimate.
        for (int blocknum = 0; blocknum < 30; blocknum++) {
            std::vector<CTransactionRef> block;
            for (int j = 0; j < 10; j++) {
                for (int k = 0; k < 10; k++) {
                    tx.vin[0].prevout.n = 10000 * blocknum + 100 * j + k;
                    mpool.addUnchecked(entry.Fee(200000 * (j + 1)).Time(GetTime()).Height(blocknum).FromTx(tx));
                    block.push_back(mpool.get(tx.GetHash()));
                }
            }
            mpool.removeForBlock(block, blocknum + 1);
        }
    }
    const CFeeRate estimate{feeEst.estimateFee(2)};
    BOOST_REQUIRE(estimate != CFeeRate(0));

    const fs::path path{m_path_root / "fee_estimates.dat"};
    {
        CAutoFile file(fsbridge::fopen(path, "wb"), SER_DISK, CLIENT_VERSION);
        BOOST_REQUIRE(feeEst.Write(file));
    }
    // What the file needs, as a later or earlier version would write it.
    const auto set_required = [&](int required) {
        CAutoFile file(fsbridge::fopen(path, "r+b"), SER_DISK, CLIENT_VERSION);
        BOOST_REQUIRE(!file.IsNull());
        file << required;
    };
    const auto read_back = [&](CBlockPolicyEstimator& est) {
        CAutoFile file(fsbridge::fopen(path, "rb"), SER_DISK, CLIENT_VERSION);
        return est.Read(file);
    };
    {
        CAutoFile file(fsbridge::fopen(path, "rb"), SER_DISK, CLIENT_VERSION);
        int required;
        file >> required;
        BOOST_CHECK_EQUAL(required, FEE_ESTIMATES_VERSION_REQUIRED);
    }

    // Written by this build: read back, whatever its CLIENT_VERSION.
    {
        CBlockPolicyEstimator est;
        BOOST_CHECK(read_back(est));
        BOOST_CHECK(est.estimateFee(2) == estimate);
    }
    // Written with the old parameters (the header of 4.22.9.5 and earlier):
    // discarded, and the estimator starts empty.
    set_required(42199);
    {
        CBlockPolicyEstimator est;
        BOOST_CHECK(read_back(est));
        BOOST_CHECK(est.estimateFee(2) == CFeeRate(0));
    }
    // One just below the version required: discarded too.
    set_required(FEE_ESTIMATES_VERSION_REQUIRED - 1);
    {
        CBlockPolicyEstimator est;
        BOOST_CHECK(read_back(est));
        BOOST_CHECK(est.estimateFee(2) == CFeeRate(0));
    }
    // Needs a later version than this build: refused.
    set_required(std::max(CLIENT_VERSION, FEE_ESTIMATES_VERSION_REQUIRED) + 1);
    {
        CBlockPolicyEstimator est;
        BOOST_CHECK(!read_back(est));
        BOOST_CHECK(est.estimateFee(2) == CFeeRate(0));
    }
}

BOOST_AUTO_TEST_SUITE_END()
