// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <policy/feerate.h>

#include <tinyformat.h>

#include <cmath>

CFeeRate::CFeeRate(const CAmount& nFeePaid, uint32_t num_bytes)
{
    const int64_t nSize{num_bytes};

    // Check if nFeePaid * 1000 would overflow int64_t before performing the multiplication.
    // Previously used MAX_MONEY as threshold, but that's a consensus rule about money supply,
    // not a mathematical overflow limit. The correct threshold is INT64_MAX / 1000.
    // INT64_MAX = 9,223,372,036,854,775,807, so INT64_MAX / 1000 = 9,223,372,036,854,775.
    // If nFeePaid exceeds this, use arbitrary precision arithmetic to prevent overflow.
    if (nFeePaid > INT64_MAX / 1000 && nSize > 0) {
        arith_uint256 nSatsPerK = arith_uint256(nFeePaid) * 1000 / arith_uint256(nSize);
        // Clamp to INT64_MAX to prevent downstream issues with double conversion
        // Check if the result fits in 63 bits (signed int64_t range)
        if (nSatsPerK.bits() > 63) {
            nSatoshisPerK = INT64_MAX;
        } else {
            nSatoshisPerK = nSatsPerK.GetLow64();
        }
    } else if (nSize > 0) {
        nSatoshisPerK = nFeePaid * 1000 / nSize;
    } else {
        nSatoshisPerK = 0;
    }
}

CAmount CFeeRate::GetFee(uint32_t num_bytes) const
{
    const int64_t nSize{num_bytes};

    // Be explicit that we're converting from a double to int64_t (CAmount) here.
    // We've previously had issues with the silent double->int64_t conversion.
    // Guard against overflow in the multiplication by using double throughout
    double dFee = std::ceil(static_cast<double>(nSatoshisPerK) * static_cast<double>(nSize) / 1000.0);

    // Clamp to valid CAmount range to prevent undefined behavior.
    // static_cast<double>(INT64_MAX) rounds up to 2^63, which is NOT representable
    // as int64_t, so a dFee of exactly 2^63 must be caught here (>=); otherwise the
    // static_cast<CAmount>(dFee) below would be undefined. INT64_MIN == -2^63 is
    // exactly representable, so the lower bound stays a strict "<".
    if (dFee >= static_cast<double>(INT64_MAX)) {
        return INT64_MAX;
    } else if (dFee < static_cast<double>(INT64_MIN)) {
        return INT64_MIN;
    }

    CAmount nFee{static_cast<CAmount>(dFee)};

    if (nFee == 0 && nSize != 0) {
        if (nSatoshisPerK > 0) nFee = CAmount(1);
        if (nSatoshisPerK < 0) nFee = CAmount(-1);
    }

    return nFee;
}

std::string CFeeRate::ToString(const FeeEstimateMode& fee_estimate_mode) const
{
    switch (fee_estimate_mode) {
    case FeeEstimateMode::SAT_VB: return strprintf("%d.%03d %s/vB", nSatoshisPerK / 1000, nSatoshisPerK % 1000, CURRENCY_ATOM);
    default:                      return strprintf("%d.%08d %s/kvB", nSatoshisPerK / COIN, nSatoshisPerK % COIN, CURRENCY_UNIT);
    }
}
