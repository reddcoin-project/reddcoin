// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Pins the integer results of the consensus code that goes through double
// arithmetic and libm: the PoSV coin-age weight (pow, log), the Kimoto
// Gravity Well event horizon (pow) and the v5 stake reward. Those doubles are not bit-identical across
// platforms (libm may differ by an ULP), but the values consensus uses are
// truncated integers and a stop/continue decision, and those must agree on
// every architecture. The expected values were generated on x86_64 and
// matched on aarch64 with and without fused multiply-add. A failure here on a
// new platform or toolchain means nodes on it would disagree with the network.

#include <chain.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <pos/kernel.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(fp_consensus_tests, BasicTestingSetup)

// GetCoinAgeWeight(begin, end) for end = begin + nStakeMinAge + seconds, using
// the mainnet parameters (8 hour minimum age, 45 day cap). Covers the cubic
// region, both sides of the switch to the log curve at 7 days, the log region
// and the approach to the cap.
BOOST_AUTO_TEST_CASE(coin_age_weight_pinned)
{
    const Consensus::Params& params = Params().GetConsensus();
    BOOST_REQUIRE_EQUAL(params.nStakeMinAge, 8 * 60 * 60);
    BOOST_REQUIRE_EQUAL(params.nStakeMaxAge, 45 * 24 * 60 * 60);

    const int64_t begin = 1400000000;
    const int64_t min_age = params.nStakeMinAge;

    struct Case {
        int64_t seconds;
        int64_t weight;
    };
    const Case cases[] = {
        {0, 0},
        {1, 1},
        {60, 60},
        {3600, 3608},
        {43200, 44390},
        {86400, 90984},
        {172800, 189727},
        {259200, 294112},
        {432000, 511346},
        {604799, 725758},
        {604800, 725760},
        {604801, 725761},
        {691200, 822671},
        {1209600, 1228818},
        {2592000, 1781949},
        {7776000, 2579278},
        {15552000, 3082336},
        {31536000, 3595405},
        {43200000, 3823810},
        // The cap is reached at exactly this many seconds of age.
        {47194879, 3887999},
        {47194880, 3888000},
    };
    for (const Case& c : cases) {
        BOOST_CHECK_MESSAGE(GetCoinAgeWeight(begin, begin + min_age + c.seconds, params) == c.weight,
                            "GetCoinAgeWeight at " << c.seconds << " s: got "
                            << GetCoinAgeWeight(begin, begin + min_age + c.seconds, params)
                            << ", expected " << c.weight);
    }

    // Younger than the minimum age, and a non-positive start time.
    BOOST_CHECK_EQUAL(GetCoinAgeWeight(begin, begin + min_age - 1, params), 0);
    BOOST_CHECK_EQUAL(GetCoinAgeWeight(0, begin, params), 0);

    // A strided sweep over every age that is not yet capped (0 to 600 days),
    // folded into a sum and an order-sensitive mix so any single changed value
    // shows up.
    uint64_t sum = 0;
    uint64_t mix = 0;
    for (int64_t s = 0; s <= int64_t{600} * 24 * 60 * 60; s += 997) {
        const int64_t w = GetCoinAgeWeight(begin, begin + min_age + s, params);
        sum += w;
        mix = (mix * 1099511628211ULL) ^ static_cast<uint64_t>(w);
    }
    BOOST_CHECK_EQUAL(sum, 168017274324ULL);
    BOOST_CHECK_EQUAL(mix, 17271307896864130344ULL);
}

// Kimoto Gravity Well stops walking back when the rate ratio
// target/actual crosses EventHorizonDeviation = 1 + 0.7084 * (mass/144)^-1.228
// (fast) or its reciprocal (slow). These chains hold that ratio near 1 for
// every block except the one at mass M, whose timestamp is chosen so the ratio
// sits on the threshold: the "stop" timestamp ends the walk at M, the timestamp
// one second the other way does not, and the walk runs to the start of the
// chain. Both outcomes are pinned through the returned compact target.
namespace {

std::vector<CBlockIndex> BuildChain(int first_height, int mass, int64_t actual_at_mass, int extra)
{
    const uint32_t tip_time = 1500000000;
    const int n = mass + extra;
    std::vector<CBlockIndex> blocks(n);
    for (int idx = 0; idx < n; ++idx) {
        const int from_tip = n - 1 - idx;
        blocks[idx].nHeight = first_height + idx;
        blocks[idx].nBits = 0x1b0404cb;
        blocks[idx].pprev = idx ? &blocks[idx - 1] : nullptr;
        blocks[idx].nTime = from_tip == mass - 1 ? tip_time - actual_at_mass : tip_time - 60 * from_tip;
    }
    return blocks;
}

} // namespace

BOOST_AUTO_TEST_CASE(kgw_event_horizon_pinned)
{
    const Consensus::Params& params = Params().GetConsensus();
    // Every chain below must stay in the proof-of-work branch of KGW.
    BOOST_REQUIRE_GT(params.nLastPowHeight, 100000 + 10020);

    struct Case {
        int first_height; // below 6000 uses the short (early chain) window
        int mass;
        int64_t fast_stop;     // largest actual time with ratio >= fast threshold
        uint32_t fast_stopped; // result with actual = fast_stop
        uint32_t fast_ran_on;  // result with actual = fast_stop + 1
        int64_t slow_stop;     // smallest actual time with ratio <= slow threshold
        uint32_t slow_stopped; // result with actual = slow_stop
        uint32_t slow_ran_on;  // result with actual = slow_stop - 1
    };
    const Case cases[] = {
        {1000, 14, 62, 0x1a4bef49, 0x1b03e688, 11254, 0x1b35d760, 0x1b03e688},
        {1000, 100, 2845, 0x1b01e7d1, 0x1b03fc38, 12652, 0x1b087961, 0x1b03fc38},
        {100000, 360, 17561, 0x1b03446b, 0x1b040215, 26567, 0x1b04f15e, 0x1b040215},
        {100000, 1440, 82924, 0x1b03db67, 0x1b040416, 90021, 0x1b042fe8, 0x1b040416},
        {100000, 10000, 597681, 0x1b0400d1, 0x1b0404b0, 602328, 0x1b0408c8, 0x1b0404b0},
    };
    const int extra = 20;
    CBlockHeader header;
    for (const Case& c : cases) {
        const auto run = [&](int64_t actual) {
            const std::vector<CBlockIndex> chain = BuildChain(c.first_height, c.mass, actual, extra);
            return GetNextWorkRequired(&chain.back(), &header, params);
        };
        BOOST_TEST_MESSAGE("KGW mass " << c.mass);
        BOOST_CHECK_EQUAL(run(c.fast_stop), c.fast_stopped);
        BOOST_CHECK_EQUAL(run(c.fast_stop + 1), c.fast_ran_on);
        BOOST_CHECK_EQUAL(run(c.slow_stop), c.slow_stopped);
        BOOST_CHECK_EQUAL(run(c.slow_stop - 1), c.slow_ran_on);
        // The threshold block decides the result.
        BOOST_CHECK(c.fast_stopped != c.fast_ran_on);
        BOOST_CHECK(c.slow_stopped != c.slow_ran_on);
    }
}

// The v5 stake reward multiplies an integer subsidy by the double inflation
// adjustment and truncates. One correctly rounded multiply, no libm, so it must
// match everywhere; pin it for the adjustment bounds (0.5, 5), 1, inexact
// values, and one shaped like GetInflationAdjustment's (float 0.05 / 12 times
// a double ratio of 21.75).
BOOST_AUTO_TEST_CASE(stake_reward_pinned)
{
    struct Case {
        int64_t coin_age;
        double adjustment;
        CAmount reward;
    };
    const Case cases[] = {
        {1, 1.0, 13689},
        {1, 0.5, 6844},
        {1, 5.0, 68445},
        {1, 0.7, 9582},
        {1, 1.2345678901234567, 16899},
        {1, 0.090625004726462066, 1240},
        {365, 1.0, 4996681},
        {365, 0.5, 2498340},
        {365, 5.0, 24983405},
        {365, 0.7, 3497676},
        {365, 1.2345678901234567, 6168741},
        {365, 0.090625004726462066, 452824},
        {366, 1.0, 5010370},
        {366, 0.5, 2505185},
        {366, 5.0, 25051850},
        {366, 0.7, 3507259},
        {366, 1.2345678901234567, 6185641},
        {366, 0.090625004726462066, 454064},
        {1000, 1.0, 13689537},
        {1000, 0.5, 6844768},
        {1000, 5.0, 68447685},
        {1000, 0.7, 9582675},
        {1000, 1.2345678901234567, 16900662},
        {1000, 0.090625004726462066, 1240614},
        {123456, 1.0, 1690055587},
        {123456, 0.5, 845027793},
        {123456, 5.0, 8450277935},
        {123456, 0.7, 1183038910},
        {123456, 1.2345678901234567, 2086488360},
        {123456, 0.090625004726462066, 153161295},
        {10000000, 1.0, 136895378743},
        {10000000, 0.5, 68447689371},
        {10000000, 5.0, 684476893715},
        {10000000, 0.7, 95826765120},
        {10000000, 1.2345678901234567, 169006638902},
        {10000000, 0.090625004726462066, 12406144345},
    };
    for (const Case& c : cases) {
        BOOST_CHECK_MESSAGE(GetProofOfStakeReward(c.coin_age, 0, c.adjustment) == c.reward,
                            "GetProofOfStakeReward(" << c.coin_age << ", 0, " << c.adjustment << ")");
    }
    // Fees are added after the truncation.
    BOOST_CHECK_EQUAL(GetProofOfStakeReward(1000, 12345, 0.7), 9582675 + 12345);
}

BOOST_AUTO_TEST_SUITE_END()
