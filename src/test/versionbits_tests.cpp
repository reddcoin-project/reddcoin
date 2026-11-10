// Copyright (c) 2014-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <deploymentstatus.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <versionbits.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <memory>
#include <vector>

/* Define a virtual block time, one block per 10 minutes after Nov 14 2014, 0:55:36am */
static int32_t TestTime(int nHeight) { return 1415926536 + 600 * nHeight; }

static const std::string StateName(ThresholdState state)
{
    switch (state) {
    case ThresholdState::DEFINED:   return "DEFINED";
    case ThresholdState::STARTED:   return "STARTED";
    case ThresholdState::LOCKED_IN: return "LOCKED_IN";
    case ThresholdState::ACTIVE:    return "ACTIVE";
    case ThresholdState::FAILED:    return "FAILED";
    } // no default case, so the compiler can warn about missing cases
    return "";
}

static const Consensus::Params paramsDummy = Consensus::Params();

class TestConditionChecker : public AbstractThresholdConditionChecker
{
private:
    mutable ThresholdConditionCache cache;

public:
    int64_t BeginTime(const Consensus::Params& params) const override { return TestTime(10000); }
    int64_t EndTime(const Consensus::Params& params) const override { return TestTime(20000); }
    int Period(const Consensus::Params& params) const override { return 1000; }
    int Threshold(const Consensus::Params& params) const override { return 900; }
    bool Condition(const CBlockIndex* pindex, const Consensus::Params& params) const override { return (pindex->nVersion & 0x100); }

    ThresholdState GetStateFor(const CBlockIndex* pindexPrev) const { return AbstractThresholdConditionChecker::GetStateFor(pindexPrev, paramsDummy, cache); }
    int GetStateSinceHeightFor(const CBlockIndex* pindexPrev) const { return AbstractThresholdConditionChecker::GetStateSinceHeightFor(pindexPrev, paramsDummy, cache); }
};

class TestDelayedActivationConditionChecker : public TestConditionChecker
{
public:
    int MinActivationHeight(const Consensus::Params& params) const override { return 15000; }
};

class TestAlwaysActiveConditionChecker : public TestConditionChecker
{
public:
    int64_t BeginTime(const Consensus::Params& params) const override { return Consensus::BIP9Deployment::ALWAYS_ACTIVE; }
};

class TestNeverActiveConditionChecker : public TestConditionChecker
{
public:
    int64_t BeginTime(const Consensus::Params& params) const override { return Consensus::BIP9Deployment::NEVER_ACTIVE; }
};

#define CHECKERS 6

class VersionBitsTester
{
    // A fake blockchain
    std::vector<CBlockIndex*> vpblock;

    // 6 independent checkers for the same bit.
    // The first one performs all checks, the second only 50%, the third only 25%, etc...
    // This is to test whether lack of cached information leads to the same results.
    TestConditionChecker checker[CHECKERS];
    // Another 6 that assume delayed activation
    TestDelayedActivationConditionChecker checker_delayed[CHECKERS];
    // Another 6 that assume always active activation
    TestAlwaysActiveConditionChecker checker_always[CHECKERS];
    // Another 6 that assume never active activation
    TestNeverActiveConditionChecker checker_never[CHECKERS];

    // Test counter (to identify failures)
    int num{1000};

public:
    VersionBitsTester& Reset() {
        // Have each group of tests be counted by the 1000s part, starting at 1000
        num = num - (num % 1000) + 1000;

        for (unsigned int i = 0; i < vpblock.size(); i++) {
            delete vpblock[i];
        }
        for (unsigned int  i = 0; i < CHECKERS; i++) {
            checker[i] = TestConditionChecker();
            checker_delayed[i] = TestDelayedActivationConditionChecker();
            checker_always[i] = TestAlwaysActiveConditionChecker();
            checker_never[i] = TestNeverActiveConditionChecker();
        }
        vpblock.clear();
        return *this;
    }

    ~VersionBitsTester() {
         Reset();
    }

    VersionBitsTester& Mine(unsigned int height, int32_t nTime, int32_t nVersion) {
        while (vpblock.size() < height) {
            CBlockIndex* pindex = new CBlockIndex();
            pindex->nHeight = vpblock.size();
            pindex->pprev = Tip();
            pindex->nTime = nTime;
            pindex->nVersion = nVersion;
            pindex->BuildSkip();
            vpblock.push_back(pindex);
        }
        return *this;
    }

    VersionBitsTester& TestStateSinceHeight(int height)
    {
        return TestStateSinceHeight(height, height);
    }

    VersionBitsTester& TestStateSinceHeight(int height, int height_delayed)
    {
        const CBlockIndex* tip = Tip();
        for (int i = 0; i < CHECKERS; i++) {
            if (InsecureRandBits(i) == 0) {
                BOOST_CHECK_MESSAGE(checker[i].GetStateSinceHeightFor(tip) == height, strprintf("Test %i for StateSinceHeight", num));
                BOOST_CHECK_MESSAGE(checker_delayed[i].GetStateSinceHeightFor(tip) == height_delayed, strprintf("Test %i for StateSinceHeight (delayed)", num));
                BOOST_CHECK_MESSAGE(checker_always[i].GetStateSinceHeightFor(tip) == 0, strprintf("Test %i for StateSinceHeight (always active)", num));
                BOOST_CHECK_MESSAGE(checker_never[i].GetStateSinceHeightFor(tip) == 0, strprintf("Test %i for StateSinceHeight (never active)", num));
            }
        }
        num++;
        return *this;
    }

    VersionBitsTester& TestState(ThresholdState exp)
    {
        return TestState(exp, exp);
    }

    VersionBitsTester& TestState(ThresholdState exp, ThresholdState exp_delayed)
    {
        if (exp != exp_delayed) {
            // only expected differences are that delayed stays in locked_in longer
            BOOST_CHECK_EQUAL(exp, ThresholdState::ACTIVE);
            BOOST_CHECK_EQUAL(exp_delayed, ThresholdState::LOCKED_IN);
        }

        const CBlockIndex* pindex = Tip();
        for (int i = 0; i < CHECKERS; i++) {
            if (InsecureRandBits(i) == 0) {
                ThresholdState got = checker[i].GetStateFor(pindex);
                ThresholdState got_delayed = checker_delayed[i].GetStateFor(pindex);
                ThresholdState got_always = checker_always[i].GetStateFor(pindex);
                ThresholdState got_never = checker_never[i].GetStateFor(pindex);
                // nHeight of the next block. If vpblock is empty, the next (ie first)
                // block should be the genesis block with nHeight == 0.
                int height = pindex == nullptr ? 0 : pindex->nHeight + 1;
                BOOST_CHECK_MESSAGE(got == exp, strprintf("Test %i for %s height %d (got %s)", num, StateName(exp), height, StateName(got)));
                BOOST_CHECK_MESSAGE(got_delayed == exp_delayed, strprintf("Test %i for %s height %d (got %s; delayed case)", num, StateName(exp_delayed), height, StateName(got_delayed)));
                BOOST_CHECK_MESSAGE(got_always == ThresholdState::ACTIVE, strprintf("Test %i for ACTIVE height %d (got %s; always active case)", num, height, StateName(got_always)));
                BOOST_CHECK_MESSAGE(got_never == ThresholdState::FAILED, strprintf("Test %i for FAILED height %d (got %s; never active case)", num, height, StateName(got_never)));
            }
        }
        num++;
        return *this;
    }

    VersionBitsTester& TestDefined() { return TestState(ThresholdState::DEFINED); }
    VersionBitsTester& TestStarted() { return TestState(ThresholdState::STARTED); }
    VersionBitsTester& TestLockedIn() { return TestState(ThresholdState::LOCKED_IN); }
    VersionBitsTester& TestActive() { return TestState(ThresholdState::ACTIVE); }
    VersionBitsTester& TestFailed() { return TestState(ThresholdState::FAILED); }

    // non-delayed should be active; delayed should still be locked in
    VersionBitsTester& TestActiveDelayed() { return TestState(ThresholdState::ACTIVE, ThresholdState::LOCKED_IN); }

    CBlockIndex* Tip() { return vpblock.empty() ? nullptr : vpblock.back(); }
};

BOOST_FIXTURE_TEST_SUITE(versionbits_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(versionbits_test)
{
    for (int i = 0; i < 64; i++) {
        // DEFINED -> STARTED after timeout reached -> FAILED
        VersionBitsTester().TestDefined().TestStateSinceHeight(0)
                           .Mine(1, TestTime(1), 0x100).TestDefined().TestStateSinceHeight(0)
                           .Mine(11, TestTime(11), 0x100).TestDefined().TestStateSinceHeight(0)
                           .Mine(989, TestTime(989), 0x100).TestDefined().TestStateSinceHeight(0)
                           .Mine(999, TestTime(20000), 0x100).TestDefined().TestStateSinceHeight(0) // Timeout and start time reached simultaneously
                           .Mine(1000, TestTime(20000), 0).TestStarted().TestStateSinceHeight(1000) // Hit started, stop signalling
                           .Mine(1999, TestTime(30001), 0).TestStarted().TestStateSinceHeight(1000)
                           .Mine(2000, TestTime(30002), 0x100).TestFailed().TestStateSinceHeight(2000) // Hit failed, start signalling again
                           .Mine(2001, TestTime(30003), 0x100).TestFailed().TestStateSinceHeight(2000)
                           .Mine(2999, TestTime(30004), 0x100).TestFailed().TestStateSinceHeight(2000)
                           .Mine(3000, TestTime(30005), 0x100).TestFailed().TestStateSinceHeight(2000)
                           .Mine(4000, TestTime(30006), 0x100).TestFailed().TestStateSinceHeight(2000)

        // DEFINED -> STARTED -> FAILED
                           .Reset().TestDefined().TestStateSinceHeight(0)
                           .Mine(1, TestTime(1), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(1000, TestTime(10000) - 1, 0x100).TestDefined().TestStateSinceHeight(0) // One second more and it would be defined
                           .Mine(2000, TestTime(10000), 0x100).TestStarted().TestStateSinceHeight(2000) // So that's what happens the next period
                           .Mine(2051, TestTime(10010), 0).TestStarted().TestStateSinceHeight(2000) // 51 old blocks
                           .Mine(2950, TestTime(10020), 0x100).TestStarted().TestStateSinceHeight(2000) // 899 new blocks
                           .Mine(3000, TestTime(20000), 0).TestFailed().TestStateSinceHeight(3000) // 50 old blocks (so 899 out of the past 1000)
                           .Mine(4000, TestTime(20010), 0x100).TestFailed().TestStateSinceHeight(3000)

        // DEFINED -> STARTED -> LOCKEDIN after timeout reached -> ACTIVE
                           .Reset().TestDefined().TestStateSinceHeight(0)
                           .Mine(1, TestTime(1), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(1000, TestTime(10000) - 1, 0x101).TestDefined().TestStateSinceHeight(0) // One second more and it would be defined
                           .Mine(2000, TestTime(10000), 0x101).TestStarted().TestStateSinceHeight(2000) // So that's what happens the next period
                           .Mine(2999, TestTime(30000), 0x100).TestStarted().TestStateSinceHeight(2000) // 999 new blocks
                           .Mine(3000, TestTime(30000), 0x100).TestLockedIn().TestStateSinceHeight(3000) // 1 new block (so 1000 out of the past 1000 are new)
                           .Mine(3999, TestTime(30001), 0).TestLockedIn().TestStateSinceHeight(3000)
                           .Mine(4000, TestTime(30002), 0).TestActiveDelayed().TestStateSinceHeight(4000, 3000)
                           .Mine(14333, TestTime(30003), 0).TestActiveDelayed().TestStateSinceHeight(4000, 3000)
                           .Mine(24000, TestTime(40000), 0).TestActive().TestStateSinceHeight(4000, 15000)

        // DEFINED -> STARTED -> LOCKEDIN before timeout -> ACTIVE
                           .Reset().TestDefined()
                           .Mine(1, TestTime(1), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(1000, TestTime(10000) - 1, 0x101).TestDefined().TestStateSinceHeight(0) // One second more and it would be defined
                           .Mine(2000, TestTime(10000), 0x101).TestStarted().TestStateSinceHeight(2000) // So that's what happens the next period
                           .Mine(2050, TestTime(10010), 0x200).TestStarted().TestStateSinceHeight(2000) // 50 old blocks
                           .Mine(2950, TestTime(10020), 0x100).TestStarted().TestStateSinceHeight(2000) // 900 new blocks
                           .Mine(2999, TestTime(19999), 0x200).TestStarted().TestStateSinceHeight(2000) // 49 old blocks
                           .Mine(3000, TestTime(29999), 0x200).TestLockedIn().TestStateSinceHeight(3000) // 1 old block (so 900 out of the past 1000)
                           .Mine(3999, TestTime(30001), 0).TestLockedIn().TestStateSinceHeight(3000)
                           .Mine(4000, TestTime(30002), 0).TestActiveDelayed().TestStateSinceHeight(4000, 3000) // delayed will not become active until height=15000
                           .Mine(14333, TestTime(30003), 0).TestActiveDelayed().TestStateSinceHeight(4000, 3000)
                           .Mine(15000, TestTime(40000), 0).TestActive().TestStateSinceHeight(4000, 15000)
                           .Mine(24000, TestTime(40000), 0).TestActive().TestStateSinceHeight(4000, 15000)

        // DEFINED multiple periods -> STARTED multiple periods -> FAILED
                           .Reset().TestDefined().TestStateSinceHeight(0)
                           .Mine(999, TestTime(999), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(1000, TestTime(1000), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(2000, TestTime(2000), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(3000, TestTime(10000), 0).TestStarted().TestStateSinceHeight(3000)
                           .Mine(4000, TestTime(10000), 0).TestStarted().TestStateSinceHeight(3000)
                           .Mine(5000, TestTime(10000), 0).TestStarted().TestStateSinceHeight(3000)
                           .Mine(5999, TestTime(20000), 0).TestStarted().TestStateSinceHeight(3000)
                           .Mine(6000, TestTime(20000), 0).TestFailed().TestStateSinceHeight(6000)
                           .Mine(7000, TestTime(20000), 0x100).TestFailed().TestStateSinceHeight(6000)
                           .Mine(24000, TestTime(20000), 0x100).TestFailed().TestStateSinceHeight(6000) // stay in FAILED no matter how much we signal
        ;
    }
}

/** Check that ComputeBlockVersion will set the appropriate bit correctly */
static void check_computeblockversion(const Consensus::Params& params, Consensus::DeploymentPos dep)
{
    // This implicitly uses g_versionbitscache, so clear it every time
    g_versionbitscache.Clear();

    int64_t bit = params.vDeployments[dep].bit;
    int64_t nStartTime = params.vDeployments[dep].nStartTime;
    int64_t nTimeout = params.vDeployments[dep].nTimeout;
    int min_activation_height = params.vDeployments[dep].min_activation_height;

    // should not be any signalling for first block
    BOOST_CHECK_EQUAL(g_versionbitscache.ComputeBlockVersion(nullptr, params), VERSIONBITS_TOP_BITS);

    // always/never active deployments shouldn't need to be tested further
    if (nStartTime == Consensus::BIP9Deployment::ALWAYS_ACTIVE ||
        nStartTime == Consensus::BIP9Deployment::NEVER_ACTIVE)
    {
        BOOST_CHECK_EQUAL(min_activation_height, 0);
        return;
    }

    BOOST_REQUIRE(nStartTime < nTimeout);
    BOOST_REQUIRE(nStartTime >= 0);
    BOOST_REQUIRE(nTimeout <= std::numeric_limits<uint32_t>::max() || nTimeout == Consensus::BIP9Deployment::NO_TIMEOUT);
    BOOST_REQUIRE(0 <= bit && bit < 32);
    // Make sure that no deployment tries to set an invalid bit.
    BOOST_REQUIRE(((1 << bit) & VERSIONBITS_TOP_MASK) == 0);
    BOOST_REQUIRE(min_activation_height >= 0);
    // Check min_activation_height is on a retarget boundary
    BOOST_REQUIRE_EQUAL(min_activation_height % params.nMinerConfirmationWindow, 0U);

    const uint32_t bitmask{g_versionbitscache.Mask(params, dep)};
    BOOST_CHECK_EQUAL(bitmask, uint32_t{1} << bit);

    // In the first chain, test that the bit is set by CBV until it has failed.
    // In the second chain, test the bit is set by CBV while STARTED and
    // LOCKED-IN, and then no longer set while ACTIVE.
    VersionBitsTester firstChain, secondChain;

    int64_t nTime = nStartTime;

    const CBlockIndex *lastBlock = nullptr;

    // Before MedianTimePast of the chain has crossed nStartTime, the bit
    // should not be set.
    if (nTime == 0) {
        // since CBlockIndex::nTime is uint32_t we can't represent any
        // earlier time, so will transition from DEFINED to STARTED at the
        // end of the first period by mining blocks at nTime == 0
        lastBlock = firstChain.Mine(params.nMinerConfirmationWindow - 1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK_EQUAL(g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit), 0);
        lastBlock = firstChain.Mine(params.nMinerConfirmationWindow, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK((g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit)) != 0);
        // then we'll keep mining at nStartTime...
    } else {
        // use a time 1s earlier than start time to check we stay DEFINED
        --nTime;

        // Start generating blocks before nStartTime
        lastBlock = firstChain.Mine(params.nMinerConfirmationWindow, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK_EQUAL(g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit), 0);

        // Mine more blocks (4 less than the adjustment period) at the old time, and check that CBV isn't setting the bit yet.
        for (uint32_t i = 1; i < params.nMinerConfirmationWindow - 4; i++) {
            lastBlock = firstChain.Mine(params.nMinerConfirmationWindow + i, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
            BOOST_CHECK_EQUAL(g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit), 0);
        }
        // Now mine 5 more blocks at the start time -- MTP should not have passed yet, so
        // CBV should still not yet set the bit.
        nTime = nStartTime;
        for (uint32_t i = params.nMinerConfirmationWindow - 4; i <= params.nMinerConfirmationWindow; i++) {
            lastBlock = firstChain.Mine(params.nMinerConfirmationWindow + i, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
            BOOST_CHECK_EQUAL(g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit), 0);
        }
        // Next we will advance to the next period and transition to STARTED,
    }

    lastBlock = firstChain.Mine(params.nMinerConfirmationWindow * 3, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
    // so ComputeBlockVersion should now set the bit,
    BOOST_CHECK((g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit)) != 0);
    // and should also be using the VERSIONBITS_TOP_BITS.
    BOOST_CHECK_EQUAL(g_versionbitscache.ComputeBlockVersion(lastBlock, params) & VERSIONBITS_TOP_MASK, VERSIONBITS_TOP_BITS);

    // Check that ComputeBlockVersion will set the bit until nTimeout
    nTime += 600;
    uint32_t blocksToMine = params.nMinerConfirmationWindow * 2; // test blocks for up to 2 time periods
    uint32_t nHeight = params.nMinerConfirmationWindow * 3;
    // These blocks are all before nTimeout is reached.
    while (nTime < nTimeout && blocksToMine > 0) {
        lastBlock = firstChain.Mine(nHeight+1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK((g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit)) != 0);
        BOOST_CHECK_EQUAL(g_versionbitscache.ComputeBlockVersion(lastBlock, params) & VERSIONBITS_TOP_MASK, VERSIONBITS_TOP_BITS);
        blocksToMine--;
        nTime += 600;
        nHeight += 1;
    }

    if (nTimeout != Consensus::BIP9Deployment::NO_TIMEOUT) {
        // can reach any nTimeout other than NO_TIMEOUT due to earlier BOOST_REQUIRE

        nTime = nTimeout;

        // finish the last period before we start timing out
        while (nHeight % params.nMinerConfirmationWindow != 0) {
            lastBlock = firstChain.Mine(nHeight+1, nTime - 1, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
            BOOST_CHECK((g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit)) != 0);
            nHeight += 1;
        }

        // FAILED is only triggered at the end of a period, so CBV should be setting
        // the bit until the period transition.
        for (uint32_t i = 0; i < params.nMinerConfirmationWindow - 1; i++) {
            lastBlock = firstChain.Mine(nHeight+1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
            BOOST_CHECK((g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit)) != 0);
            nHeight += 1;
        }
        // The next block should trigger no longer setting the bit.
        lastBlock = firstChain.Mine(nHeight+1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK_EQUAL(g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit), 0);
    }

    // On a new chain:
    // verify that the bit will be set after lock-in, and then stop being set
    // after activation.
    nTime = nStartTime;

    // Mine one period worth of blocks, and check that the bit will be on for the
    // next period.
    lastBlock = secondChain.Mine(params.nMinerConfirmationWindow, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
    BOOST_CHECK((g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit)) != 0);

    // Mine another period worth of blocks, signaling the new bit.
    lastBlock = secondChain.Mine(params.nMinerConfirmationWindow * 2, nTime, VERSIONBITS_TOP_BITS | (1<<bit)).Tip();
    // After one period of setting the bit on each block, it should have locked in.
    // We keep setting the bit for one more period though, until activation.
    BOOST_CHECK((g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit)) != 0);

    // Now check that we keep mining the block until the end of this period, and
    // then stop at the beginning of the next period.
    lastBlock = secondChain.Mine((params.nMinerConfirmationWindow * 3) - 1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
    BOOST_CHECK((g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit)) != 0);
    lastBlock = secondChain.Mine(params.nMinerConfirmationWindow * 3, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();

    if (lastBlock->nHeight + 1 < min_activation_height) {
        // check signalling continues while min_activation_height is not reached
        lastBlock = secondChain.Mine(min_activation_height - 1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK((g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit)) != 0);
        // then reach min_activation_height, which was already REQUIRE'd to start a new period
        lastBlock = secondChain.Mine(min_activation_height, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
    }

    // Check that we don't signal after activation
    BOOST_CHECK_EQUAL(g_versionbitscache.ComputeBlockVersion(lastBlock, params) & (1 << bit), 0);
}

BOOST_AUTO_TEST_CASE(versionbits_computeblockversion)
{
    // check that any deployment on any chain can conceivably reach both
    // ACTIVE and FAILED states in roughly the way we expect
    for (const auto& chain_name : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::SIGNET, CBaseChainParams::REGTEST}) {
        const auto chainParams = CreateChainParams(*m_node.args, chain_name);
        uint32_t chain_all_vbits{0};
        for (int i = 0; i < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++i) {
            const auto dep = static_cast<Consensus::DeploymentPos>(i);
            // Check that no bits are re-used (within the same chain). This is
            // disallowed because the transition to FAILED (on timeout) does
            // not take precedence over STARTED/LOCKED_IN. So all softforks on
            // the same bit might overlap, even when non-overlapping start-end
            // times are picked.
            const uint32_t dep_mask{g_versionbitscache.Mask(chainParams->GetConsensus(), dep)};
            BOOST_CHECK(!(chain_all_vbits & dep_mask));
            chain_all_vbits |= dep_mask;
            check_computeblockversion(chainParams->GetConsensus(), dep);
        }
    }

    {
        // Use regtest/testdummy to ensure we always exercise some
        // deployment that's not always/never active
        ArgsManager args;
        args.ForceSetArg("-vbparams", "testdummy:1199145601:1230767999"); // January 1, 2008 - December 31, 2008
        const auto chainParams = CreateChainParams(args, CBaseChainParams::REGTEST);
        check_computeblockversion(chainParams->GetConsensus(), Consensus::DEPLOYMENT_TESTDUMMY);
    }

    {
        // Use regtest/testdummy to ensure we always exercise the
        // min_activation_height test, even if we're not using that in a
        // live deployment
        ArgsManager args;
        args.ForceSetArg("-vbparams", "testdummy:1199145601:1230767999:403200"); // January 1, 2008 - December 31, 2008, min act height 403200
        const auto chainParams = CreateChainParams(args, CBaseChainParams::REGTEST);
        check_computeblockversion(chainParams->GetConsensus(), Consensus::DEPLOYMENT_TESTDUMMY);
    }
}

/** Count signalling for the period holding the block after tip, and the one before, from scratch. */
static std::pair<SignallingCounts, SignallingCounts> RecountSignalling(const CBlockIndex* tip, int period)
{
    std::pair<SignallingCounts, SignallingCounts> result;
    const int current_start = (tip->nHeight + 1) / period * period;
    for (const CBlockIndex* b = tip; b != nullptr; b = b->pprev) {
        SignallingCounts* counts{nullptr};
        if (b->nHeight >= current_start) {
            counts = &result.first;
        } else if (b->nHeight >= current_start - period) {
            counts = &result.second;
        } else {
            break;
        }
        ++counts->elapsed;
        if ((b->nVersion & VERSIONBITS_TOP_MASK) != VERSIONBITS_TOP_BITS) continue;
        for (int bit = 0; bit < VERSIONBITS_NUM_BITS; ++bit) {
            if ((b->nVersion >> bit) & 1) ++counts->count[bit];
        }
    }
    return result;
}

static void CheckSignalling(VersionBitsCache& cache, const CBlockIndex* tip, const Consensus::Params& params)
{
    const auto [current, previous] = cache.Signalling(tip, params);
    const auto [want_current, want_previous] = RecountSignalling(tip, params.nMinerConfirmationWindow);
    BOOST_CHECK_EQUAL(current.elapsed, want_current.elapsed);
    BOOST_CHECK_EQUAL(previous.elapsed, want_previous.elapsed);
    BOOST_CHECK(current.count == want_current.count);
    BOOST_CHECK(previous.count == want_previous.count);
}

BOOST_AUTO_TEST_CASE(versionbits_signalling_counts)
{
    Consensus::Params params;
    params.nMinerConfirmationWindow = 10;

    // Build a chain from a list of versions; fork_from is the parent of the first block.
    std::vector<std::unique_ptr<CBlockIndex>> blocks;
    auto build = [&](const CBlockIndex* fork_from, const std::vector<int32_t>& versions) {
        std::vector<const CBlockIndex*> chain;
        CBlockIndex* prev = const_cast<CBlockIndex*>(fork_from);
        for (int32_t version : versions) {
            blocks.push_back(std::make_unique<CBlockIndex>());
            CBlockIndex* b = blocks.back().get();
            b->pprev = prev;
            b->nHeight = prev ? prev->nHeight + 1 : 0;
            b->nVersion = version;
            b->BuildSkip();
            chain.push_back(b);
            prev = b;
        }
        return chain;
    };
    const int32_t signal5{VERSIONBITS_TOP_BITS | (1 << 5)};
    const int32_t signal5_27{VERSIONBITS_TOP_BITS | (1 << 5) | (1 << 27)};
    std::vector<int32_t> versions;
    for (int i = 0; i < 35; ++i) {
        // A mix of plain versionbits, signalling and old-format blocks.
        versions.push_back(i % 3 == 0 ? signal5 : i % 4 == 0 ? signal5_27 : i % 5 == 0 ? 4 : VERSIONBITS_TOP_BITS);
    }
    const auto chain_a = build(nullptr, versions);

    VersionBitsCache cache;
    // Extending one block at a time, across period boundaries.
    for (const CBlockIndex* tip : chain_a) CheckSignalling(cache, tip, params);

    // A reorg to a fork from height 17, all blocks setting bit 27.
    const auto chain_b = build(chain_a[17], std::vector<int32_t>(20, signal5_27));
    for (const CBlockIndex* tip : chain_b) CheckSignalling(cache, tip, params);

    // Back to the first chain, then jumps of several blocks at once.
    CheckSignalling(cache, chain_a[20], params);
    CheckSignalling(cache, chain_a[24], params);
    CheckSignalling(cache, chain_a[29], params);
    CheckSignalling(cache, chain_a[34], params);
    // Going back within the same period is a reorg too.
    CheckSignalling(cache, chain_a[31], params);

    // The first period has no previous one.
    VersionBitsCache fresh;
    const auto [current, previous] = fresh.Signalling(chain_a[4], params);
    BOOST_CHECK_EQUAL(current.elapsed, 5);
    BOOST_CHECK_EQUAL(previous.elapsed, 0);
}

/** Count signalling for each whole period before the one holding the block after tip, from scratch, oldest first. */
static std::vector<SignallingCounts> RecountHistory(const CBlockIndex* tip, int period, int periods)
{
    const int current_start = (tip->nHeight + 1) / period * period;
    const int first = std::max(0, current_start / period - periods);
    std::vector<SignallingCounts> history(current_start / period - first);
    for (const CBlockIndex* b = tip; b != nullptr && b->nHeight >= first * period; b = b->pprev) {
        if (b->nHeight >= current_start) continue;
        SignallingCounts& counts = history[b->nHeight / period - first];
        ++counts.elapsed;
        if ((b->nVersion & VERSIONBITS_TOP_MASK) != VERSIONBITS_TOP_BITS) continue;
        for (int bit = 0; bit < VERSIONBITS_NUM_BITS; ++bit) {
            if ((b->nVersion >> bit) & 1) ++counts.count[bit];
        }
    }
    return history;
}

static void CheckHistory(VersionBitsCache& cache, const CBlockIndex* tip, const Consensus::Params& params, int periods)
{
    const std::vector<SignallingCounts> history = cache.SignallingHistory(tip, params, periods);
    const std::vector<SignallingCounts> want = RecountHistory(tip, params.nMinerConfirmationWindow, periods);
    BOOST_REQUIRE_EQUAL(history.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        BOOST_CHECK_EQUAL(history[i].elapsed, params.nMinerConfirmationWindow);
        BOOST_CHECK_EQUAL(history[i].elapsed, want[i].elapsed);
        BOOST_CHECK(history[i].count == want[i].count);
    }
    // The last completed period is the previous one of Signalling().
    if (!history.empty()) {
        BOOST_CHECK(history.back().count == cache.Signalling(tip, params).second.count);
    }
}

BOOST_AUTO_TEST_CASE(versionbits_signalling_history)
{
    Consensus::Params params;
    params.nMinerConfirmationWindow = 10;

    std::vector<std::unique_ptr<CBlockIndex>> blocks;
    auto build = [&](const CBlockIndex* fork_from, const std::vector<int32_t>& versions) {
        std::vector<const CBlockIndex*> chain;
        CBlockIndex* prev = const_cast<CBlockIndex*>(fork_from);
        for (int32_t version : versions) {
            blocks.push_back(std::make_unique<CBlockIndex>());
            CBlockIndex* b = blocks.back().get();
            b->pprev = prev;
            b->nHeight = prev ? prev->nHeight + 1 : 0;
            b->nVersion = version;
            b->BuildSkip();
            chain.push_back(b);
            prev = b;
        }
        return chain;
    };
    // Support for bit 5 that grows period by period, among old-format blocks.
    std::vector<int32_t> versions;
    for (int i = 0; i < 95; ++i) {
        const bool signals = (i % 10) < i / 10;
        versions.push_back(i % 7 == 6 ? 4 : signals ? VERSIONBITS_TOP_BITS | (1 << 5) : VERSIONBITS_TOP_BITS);
    }
    const auto chain_a = build(nullptr, versions);

    VersionBitsCache cache;
    // No completed period yet, then one more at each boundary, up to the limit.
    BOOST_CHECK(cache.SignallingHistory(chain_a[5], params, 4).empty());
    for (const CBlockIndex* tip : chain_a) CheckHistory(cache, tip, params, 4);
    BOOST_CHECK_EQUAL(cache.SignallingHistory(chain_a[94], params, 4).size(), 4U);
    BOOST_CHECK_EQUAL(cache.SignallingHistory(chain_a[94], params, 20).size(), 9U);
    BOOST_CHECK(cache.SignallingHistory(chain_a[94], params, 0).empty());

    // A reorg that replaces the last two whole periods: the cached counts of
    // the old ones must not be used for the new ones.
    const auto chain_b = build(chain_a[69], std::vector<int32_t>(25, VERSIONBITS_TOP_BITS | (1 << 27)));
    for (const CBlockIndex* tip : chain_b) CheckHistory(cache, tip, params, 4);
    CheckHistory(cache, chain_a[94], params, 4);
    CheckHistory(cache, chain_a[50], params, 8);
}

BOOST_AUTO_TEST_SUITE_END()
