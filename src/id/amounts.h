// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ID_AMOUNTS_H
#define BITCOIN_ID_AMOUNTS_H

#include <amount.h>

#include <cstdint>
#include <limits>

// Shares of amounts: deposits, minimum bids, renewal fees and the split of
// the proceeds of an auction. Everything here is integer arithmetic, so that
// every build of the node, and every command that asks the node for an
// amount, gets the same number. Floating point does not: a build that
// evaluates double expressions with extended precision (32-bit x86 with the
// x87 unit) gets amounts that are one base unit lower for some inputs, and
// above 2^53 base units a double cannot hold an amount at all.
//
// Rounding rule: the exact result is rounded toward zero, which is down for
// the amounts and shares that the node uses, none of which is negative. A
// minimum bid of 5% over 5000.00000001 RDD is 5250.00000001 RDD and not
// 5250.00000002.

/** A percentage is held in hundredths of a percent: 525 is 5.25%. */
static constexpr int32_t REDDID_HUNDREDTHS_PER_PERCENT = 100;
/** The number of hundredths of a percent in the whole. */
static constexpr int32_t REDDID_HUNDREDTHS_IN_WHOLE = 100 * REDDID_HUNDREDTHS_PER_PERCENT;
/** The minimum increment of a bid where no namespace sets one: 5%. */
static constexpr int32_t REDDID_DEFAULT_BID_INCREMENT_HUNDREDTHS = 5 * REDDID_HUNDREDTHS_PER_PERCENT;

/**
 * amount * numerator / denominator, rounded toward zero.
 *
 * The product is never formed, so nothing overflows for any amount, MAX_MONEY
 * and the whole range of CAmount included. A result that CAmount cannot hold
 * is the largest or the smallest amount that it can. A denominator that is
 * not positive gives 0.
 */
constexpr CAmount MulDivAmount(CAmount amount, int32_t numerator, int32_t denominator)
{
    if (denominator <= 0) return 0;

    const bool negative = (amount < 0) != (numerator < 0);
    // The magnitudes, as unsigned numbers. -x cannot be formed for the
    // smallest amount: its magnitude is the complement and one, which, like
    // everything else here, neither overflows nor wraps.
    const uint64_t a = amount < 0 ? ~static_cast<uint64_t>(amount) + uint64_t{1} : static_cast<uint64_t>(amount);
    const uint64_t n = static_cast<uint64_t>(numerator < 0 ? -int64_t{numerator} : int64_t{numerator});
    const uint64_t d = static_cast<uint64_t>(denominator);

    // a * n / d = (a / d) * n + (a % d) * n / d, and the last term is rounded
    // down. Its first factor is below 2^31 and its second is 2^31 at most, so
    // it cannot overflow.
    const uint64_t whole = a / d;
    const uint64_t part = (a % d) * n / d;

    const uint64_t limit = static_cast<uint64_t>(std::numeric_limits<CAmount>::max()) + (negative ? uint64_t{1} : uint64_t{0});
    const uint64_t magnitude = (n != 0 && whole > (limit - part) / n) ? limit : whole * n + part;

    if (!negative) return static_cast<CAmount>(magnitude);
    // -magnitude, without forming 2^63 as a signed number
    return magnitude == 0 ? 0 : -static_cast<CAmount>(magnitude - 1) - 1;
}

/** The sum of two amounts, or the largest or smallest amount if CAmount cannot hold it. */
constexpr CAmount SaturatingAddAmount(CAmount a, CAmount b)
{
    if (b > 0 && a > std::numeric_limits<CAmount>::max() - b) return std::numeric_limits<CAmount>::max();
    if (b < 0 && a < std::numeric_limits<CAmount>::min() - b) return std::numeric_limits<CAmount>::min();
    return a + b;
}

/** A whole percentage of an amount, rounded toward zero. */
constexpr CAmount PercentOfAmount(CAmount amount, int32_t percent)
{
    return MulDivAmount(amount, percent, 100);
}

/** A share of an amount that is given in hundredths of a percent, rounded toward zero. */
constexpr CAmount HundredthsOfAmount(CAmount amount, int32_t hundredths)
{
    return MulDivAmount(amount, hundredths, REDDID_HUNDREDTHS_IN_WHOLE);
}

/**
 * A percentage that is held as a double, in hundredths of a percent.
 *
 * NamespaceInfo::minBidIncrement is a double and is written to the database
 * and sent to peers as this integer. The serialization of NamespaceInfo calls
 * this function, and so does every calculation with the percentage: what a
 * node computes with is what it stores and what its peers receive.
 *
 * This is the one place where a share is still a double. The two results are
 * stored in volatile doubles so that a build with extended precision rounds
 * each of them to double precision as every other build does: the product of
 * a double and 100 and that sum are exact in extended precision, so they are
 * rounded once, to the same double. Without that, such a build converts some
 * percentages with three decimals, 2.495 for one, to another integer.
 *
 * A percentage that the integer cannot hold is the largest or the smallest
 * integer, and one that is not a number is 0.
 */
inline int32_t PercentToHundredths(double percent)
{
    const volatile double scaled = percent * 100.0;
    const volatile double rounded = scaled + 0.5;  // Round to nearest
    const double value = rounded;
    if (value != value) return 0;
    if (value >= 2147483647.0) return std::numeric_limits<int32_t>::max();
    if (value <= -2147483648.0) return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}

/**
 * The lowest bid that is accepted over a current bid: the current bid and
 * the increment on it, which is given in hundredths of a percent and is
 * rounded toward zero.
 */
constexpr CAmount MinimumNextBid(CAmount currentBid, int32_t incrementHundredths)
{
    return SaturatingAddAmount(currentBid, HundredthsOfAmount(currentBid, incrementHundredths));
}

#endif // BITCOIN_ID_AMOUNTS_H
