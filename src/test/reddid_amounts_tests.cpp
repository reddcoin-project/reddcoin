// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/amounts.h>
#include <id/reddid.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <version.h>

#include <cstdint>
#include <initializer_list>
#include <limits>

// The integer arithmetic for shares of amounts in id/amounts.h. No assertion
// here depends on the build: the same file passes on 32-bit x86 with the x87
// unit, where the floating point arithmetic that this replaced gave amounts
// that were one base unit lower.

namespace {
const CAmount AMOUNT_MAX = std::numeric_limits<CAmount>::max();
const CAmount AMOUNT_MIN = std::numeric_limits<CAmount>::min();
const CAmount TWO_53 = CAmount{1} << 53;

// The results are constants: the compiler evaluates them too
static_assert(PercentOfAmount(10000 * COIN, 15) == 1500 * COIN, "15% of 10000 RDD");
static_assert(MinimumNextBid(10000 * COIN, 500) == 10500 * COIN, "5% over 10000 RDD");
static_assert(MulDivAmount(AMOUNT_MAX, 1, 1) == AMOUNT_MAX, "the whole range");
} // namespace

BOOST_FIXTURE_TEST_SUITE(reddid_amounts_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(share_is_rounded_toward_zero)
{
    BOOST_CHECK_EQUAL(MulDivAmount(0, 5, 100), 0);
    BOOST_CHECK_EQUAL(MulDivAmount(100, 0, 100), 0);
    BOOST_CHECK_EQUAL(MulDivAmount(100, 5, 100), 5);
    // 19 * 5 / 100 = 0.95, 20 * 5 / 100 = 1, 39 * 5 / 100 = 1.95
    BOOST_CHECK_EQUAL(MulDivAmount(19, 5, 100), 0);
    BOOST_CHECK_EQUAL(MulDivAmount(20, 5, 100), 1);
    BOOST_CHECK_EQUAL(MulDivAmount(39, 5, 100), 1);
    BOOST_CHECK_EQUAL(MulDivAmount(40, 5, 100), 2);
    // One base unit below and above a whole result
    BOOST_CHECK_EQUAL(MulDivAmount(5000 * COIN - 1, 5, 100), 250 * COIN - 1);
    BOOST_CHECK_EQUAL(MulDivAmount(5000 * COIN, 5, 100), 250 * COIN);
    BOOST_CHECK_EQUAL(MulDivAmount(5000 * COIN + 19, 5, 100), 250 * COIN);
    BOOST_CHECK_EQUAL(MulDivAmount(5000 * COIN + 20, 5, 100), 250 * COIN + 1);
    // More than the whole
    BOOST_CHECK_EQUAL(MulDivAmount(7, 3, 2), 10);
    BOOST_CHECK_EQUAL(MulDivAmount(1000, 250, 100), 2500);

    // Toward zero for the amounts and shares that are negative, which the
    // node does not use: the result of the same sizes with a sign
    BOOST_CHECK_EQUAL(MulDivAmount(-19, 5, 100), 0);
    BOOST_CHECK_EQUAL(MulDivAmount(-39, 5, 100), -1);
    BOOST_CHECK_EQUAL(MulDivAmount(39, -5, 100), -1);
    BOOST_CHECK_EQUAL(MulDivAmount(-39, -5, 100), 1);
    BOOST_CHECK_EQUAL(MulDivAmount(-7, 3, 2), -10);
    BOOST_CHECK_EQUAL(MulDivAmount(-100, 5, 100), -5);
    BOOST_CHECK_EQUAL(MulDivAmount(-1, 1, 1), -1);
    BOOST_CHECK_EQUAL(MulDivAmount(1, std::numeric_limits<int32_t>::min(), 1), std::numeric_limits<int32_t>::min());
    BOOST_CHECK_EQUAL(MulDivAmount(std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()), std::numeric_limits<int32_t>::min());

    // No share of anything without a positive denominator
    BOOST_CHECK_EQUAL(MulDivAmount(1000, 5, 0), 0);
    BOOST_CHECK_EQUAL(MulDivAmount(1000, 5, -100), 0);
}

BOOST_AUTO_TEST_CASE(share_of_a_large_amount_is_exact)
{
    // Above 2^53 a double holds every second whole number at best.
    // 9007199254740993 * 5 / 100 = 450359962737049.65
    BOOST_CHECK_EQUAL(MulDivAmount(TWO_53 + 1, 5, 100), 450359962737049);
    BOOST_CHECK_EQUAL(MulDivAmount(TWO_53 + 7, 5, 100), 450359962737049);
    BOOST_CHECK_EQUAL(MulDivAmount(TWO_53 + 8, 5, 100), 450359962737050);
    BOOST_CHECK_EQUAL(MulDivAmount(10000000000000001, 500, 10000), 500000000000000);
    BOOST_CHECK_EQUAL(MulDivAmount(10000000000000003, 500, 10000), 500000000000000);
    BOOST_CHECK_EQUAL(MulDivAmount(10000000000000020, 500, 10000), 500000000000001);

    // All the money there can be, and all that an amount can hold: the
    // product does not fit and is never formed
    BOOST_CHECK_EQUAL(MAX_MONEY, 9223372036800000000);
    BOOST_CHECK_EQUAL(MulDivAmount(MAX_MONEY, 15, 100), 1383505805520000000);
    BOOST_CHECK_EQUAL(MulDivAmount(MAX_MONEY, 70, 100), 6456360425760000000);
    BOOST_CHECK_EQUAL(MulDivAmount(MAX_MONEY, 100, 100), MAX_MONEY);
    BOOST_CHECK_EQUAL(MulDivAmount(MAX_MONEY, 755, 10000), 696364588778400000);
    BOOST_CHECK_EQUAL(MulDivAmount(MAX_MONEY, 85, 400), 1959966557820000000);
    // 9223372036854775807 * 15 / 100 = 1383505805528216371.05
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MAX, 15, 100), 1383505805528216371);
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MAX, 99, 100), 9131138316486228048);
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MAX, 100, 100), AMOUNT_MAX);
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MAX, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()), AMOUNT_MAX);
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MIN, 100, 100), AMOUNT_MIN);
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MIN, 50, 100), AMOUNT_MIN / 2);
}

BOOST_AUTO_TEST_CASE(share_that_does_not_fit_is_the_limit)
{
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MAX, 101, 100), AMOUNT_MAX);
    BOOST_CHECK_EQUAL(MulDivAmount(MAX_MONEY, 10500, 10000), AMOUNT_MAX);
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MAX, std::numeric_limits<int32_t>::max(), 1), AMOUNT_MAX);
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MIN, 101, 100), AMOUNT_MIN);
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MAX, -101, 100), AMOUNT_MIN);
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MIN, -1, 1), AMOUNT_MAX);
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MAX, std::numeric_limits<int32_t>::min(), 1), AMOUNT_MIN);
    // The last result that fits and the first that does not
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MAX / 2, 2, 1), AMOUNT_MAX - 1);
    BOOST_CHECK_EQUAL(MulDivAmount(AMOUNT_MAX / 2 + 1, 2, 1), AMOUNT_MAX);

    BOOST_CHECK_EQUAL(SaturatingAddAmount(1, 2), 3);
    BOOST_CHECK_EQUAL(SaturatingAddAmount(AMOUNT_MAX - 1, 1), AMOUNT_MAX);
    BOOST_CHECK_EQUAL(SaturatingAddAmount(AMOUNT_MAX, 1), AMOUNT_MAX);
    BOOST_CHECK_EQUAL(SaturatingAddAmount(AMOUNT_MAX, AMOUNT_MAX), AMOUNT_MAX);
    BOOST_CHECK_EQUAL(SaturatingAddAmount(AMOUNT_MIN, -1), AMOUNT_MIN);
    BOOST_CHECK_EQUAL(SaturatingAddAmount(AMOUNT_MIN, AMOUNT_MAX), -1);
}

BOOST_AUTO_TEST_CASE(percentages)
{
    BOOST_CHECK_EQUAL(PercentOfAmount(10000 * COIN, 10), 1000 * COIN);
    BOOST_CHECK_EQUAL(PercentOfAmount(10000 * COIN, 15), 1500 * COIN);
    BOOST_CHECK_EQUAL(PercentOfAmount(10000 * COIN, 20), 2000 * COIN);
    BOOST_CHECK_EQUAL(PercentOfAmount(10499 * COIN, 15), 157485000000);
    BOOST_CHECK_EQUAL(PercentOfAmount(9001, 50), 4500);
    BOOST_CHECK_EQUAL(PercentOfAmount(0, 15), 0);
    BOOST_CHECK_EQUAL(PercentOfAmount(6, 15), 0);
    BOOST_CHECK_EQUAL(PercentOfAmount(7, 15), 1);
    BOOST_CHECK_EQUAL(PercentOfAmount(1000, 0), 0);
    BOOST_CHECK_EQUAL(PercentOfAmount(1000, 100), 1000);

    // The four shares of the proceeds of an auction never add up to more than
    // the bid
    for (CAmount bid : {CAmount{1}, CAmount{99}, 10000 * COIN + 33, TWO_53 + 1, MAX_MONEY, AMOUNT_MAX}) {
        const CAmount shares = PercentOfAmount(bid, 70) + PercentOfAmount(bid, 10) + PercentOfAmount(bid, 15) + PercentOfAmount(bid, 5);
        BOOST_CHECK(shares <= bid);
        BOOST_CHECK(shares >= bid - 3);
    }

    BOOST_CHECK_EQUAL(HundredthsOfAmount(10000 * COIN, 500), 500 * COIN);
    BOOST_CHECK_EQUAL(HundredthsOfAmount(10000 * COIN, 755), 755 * COIN);
    BOOST_CHECK_EQUAL(HundredthsOfAmount(10000 * COIN, 1), COIN);
    // 0.01% of 9999 is 0.9999
    BOOST_CHECK_EQUAL(HundredthsOfAmount(9999, 1), 0);
    BOOST_CHECK_EQUAL(HundredthsOfAmount(10000, 1), 1);
}

BOOST_AUTO_TEST_CASE(percentage_in_hundredths)
{
    BOOST_CHECK_EQUAL(PercentToHundredths(0.0), 0);
    BOOST_CHECK_EQUAL(PercentToHundredths(1.0), 100);
    BOOST_CHECK_EQUAL(PercentToHundredths(5.0), 500);
    BOOST_CHECK_EQUAL(PercentToHundredths(5.25), 525);
    BOOST_CHECK_EQUAL(PercentToHundredths(7.55), 755);
    BOOST_CHECK_EQUAL(PercentToHundredths(20.0), 2000);
    // To the nearest hundredth
    BOOST_CHECK_EQUAL(PercentToHundredths(2.499), 250);
    BOOST_CHECK_EQUAL(PercentToHundredths(2.494), 249);
    // 2.495 is no double. The nearest one is below it, and the product with
    // 100 is rounded to 249.5 in double precision.
    BOOST_CHECK_EQUAL(PercentToHundredths(2.495), 250);

    // What the integer cannot hold is its limit, and what is no number is 0
    BOOST_CHECK_EQUAL(PercentToHundredths(21474836.47), std::numeric_limits<int32_t>::max());
    BOOST_CHECK_EQUAL(PercentToHundredths(21474836.48), std::numeric_limits<int32_t>::max());
    BOOST_CHECK_EQUAL(PercentToHundredths(1e300), std::numeric_limits<int32_t>::max());
    BOOST_CHECK_EQUAL(PercentToHundredths(std::numeric_limits<double>::infinity()), std::numeric_limits<int32_t>::max());
    BOOST_CHECK_EQUAL(PercentToHundredths(-21474836.49), std::numeric_limits<int32_t>::min());
    BOOST_CHECK_EQUAL(PercentToHundredths(-1e300), std::numeric_limits<int32_t>::min());
    BOOST_CHECK_EQUAL(PercentToHundredths(-std::numeric_limits<double>::infinity()), std::numeric_limits<int32_t>::min());
    BOOST_CHECK_EQUAL(PercentToHundredths(std::numeric_limits<double>::quiet_NaN()), 0);
    BOOST_CHECK_EQUAL(PercentToHundredths(21474836.0), 2147483600);
    // Negative percentages are cut off toward zero after the half is added
    BOOST_CHECK_EQUAL(PercentToHundredths(-5.0), -499);
    BOOST_CHECK_EQUAL(PercentToHundredths(-0.004), 0);

    // Every percentage that was stored or received is a number of hundredths
    // divided by 100, and converts back to that number
    for (int32_t hundredths = 0; hundredths <= 100000; ++hundredths) {
        BOOST_REQUIRE_EQUAL(PercentToHundredths(hundredths / 100.0), hundredths);
    }

    // The serialization of a namespace writes that integer
    for (double percent : {5.0, 7.55, 2.499, 2.495, 12.345}) {
        NamespaceInfo info;
        info.minBidIncrement = percent;
        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << info;
        NamespaceInfo read;
        stream >> read;
        BOOST_CHECK_EQUAL(PercentToHundredths(read.minBidIncrement), PercentToHundredths(percent));
    }
}

BOOST_AUTO_TEST_CASE(minimum_next_bid)
{
    BOOST_CHECK_EQUAL(MinimumNextBid(10000 * COIN, 500), 10500 * COIN);
    BOOST_CHECK_EQUAL(MinimumNextBid(5000 * COIN, 500), 5250 * COIN);
    BOOST_CHECK_EQUAL(MinimumNextBid(5000 * COIN, REDDID_DEFAULT_BID_INCREMENT_HUNDREDTHS), 5250 * COIN);
    // The increment is rounded down: 5% of 500000000001 is 25000000000.05
    BOOST_CHECK_EQUAL(MinimumNextBid(5000 * COIN + 1, 500), 5250 * COIN + 1);
    BOOST_CHECK_EQUAL(MinimumNextBid(5000 * COIN + 19, 500), 5250 * COIN + 19);
    BOOST_CHECK_EQUAL(MinimumNextBid(5000 * COIN + 20, 500), 5250 * COIN + 21);
    // An increment that rounds down to nothing asks for the same bid again
    BOOST_CHECK_EQUAL(MinimumNextBid(0, 500), 0);
    BOOST_CHECK_EQUAL(MinimumNextBid(1, 500), 1);
    BOOST_CHECK_EQUAL(MinimumNextBid(19, 500), 19);
    BOOST_CHECK_EQUAL(MinimumNextBid(20, 500), 21);

    // A percentage with decimals: 7.55% of 500000000001 is 37750000000.0755
    BOOST_CHECK_EQUAL(MinimumNextBid(5000 * COIN, PercentToHundredths(7.55)), 537750000000);
    BOOST_CHECK_EQUAL(MinimumNextBid(5000 * COIN + 1, PercentToHundredths(7.55)), 537750000001);
    BOOST_CHECK_EQUAL(MinimumNextBid(10000 * COIN, PercentToHundredths(2.5)), 10250 * COIN);
    BOOST_CHECK_EQUAL(MinimumNextBid(10000 * COIN, PercentToHundredths(1.0)), 10100 * COIN);
    BOOST_CHECK_EQUAL(MinimumNextBid(10000 * COIN, PercentToHundredths(20.0)), 12000 * COIN);

    // Above 2^53
    BOOST_CHECK_EQUAL(MinimumNextBid(10000000000000001, 500), 10500000000000001);
    BOOST_CHECK_EQUAL(MinimumNextBid(10000000000000003, 500), 10500000000000003);
    BOOST_CHECK_EQUAL(MinimumNextBid(TWO_53 + 1, 500), 9457559217478042);
    // Close to the largest minimum that an amount can hold
    BOOST_CHECK_EQUAL(MinimumNextBid(8784163844623596000, 500), 9223372036854775800);
    BOOST_CHECK_EQUAL(MinimumNextBid(MAX_MONEY, 500), AMOUNT_MAX);
    BOOST_CHECK_EQUAL(MinimumNextBid(AMOUNT_MAX, 500), AMOUNT_MAX);
    BOOST_CHECK_EQUAL(MinimumNextBid(AMOUNT_MAX, 0), AMOUNT_MAX);
}

BOOST_AUTO_TEST_SUITE_END()
