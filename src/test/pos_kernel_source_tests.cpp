// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <amount.h>
#include <array>
#include <node/transaction.h>
#include <pos/kernel.h>
#include <primitives/transaction.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

//! The stake kernel reads four things about the coin it hashes: the hash and
//! time of the block that created it, the creating transaction's timestamp,
//! and the coin's value. The block files hold all four, and so do the UTXO set
//! and the block index. The staker is moving to the second source because the
//! first costs a disk read per coin per pass; the two must agree exactly, or
//! the staker would search against a kernel no validator would accept.
BOOST_FIXTURE_TEST_SUITE(pos_kernel_source_tests, TestChain100Setup)

//! Every unspent output on the fixture's chain, compared field by field
//! between the two sources.
BOOST_AUTO_TEST_CASE(utxo_source_matches_disk)
{
    LOCK(cs_main);
    CChainState& chainstate = m_node.chainman->ActiveChainstate();
    CCoinsViewCache& view = chainstate.CoinsTip();

    size_t checked = 0;

    for (const CTransactionRef& tx : m_coinbase_txns) {
        for (uint32_t n = 0; n < tx->vout.size(); ++n) {
            const COutPoint outpoint{tx->GetHash(), n};
            const Coin& coin = view.AccessCoin(outpoint);
            if (coin.IsSpent()) continue;

            // The source the staker is moving to.
            StakeKernelSource from_utxo;
            BOOST_REQUIRE(GetStakeKernelSource(&chainstate, view, outpoint, from_utxo));

            // The source it uses today, and the one the validator keeps
            // using: the block header and the transaction, off disk.
            CTransactionRef txPrev;
            CBlockHeader blockFrom;
            unsigned int nTxOffset = 0;
            BOOST_REQUIRE(GetTransaction(&chainstate, outpoint, txPrev, blockFrom, nTxOffset));

            BOOST_CHECK_EQUAL(from_utxo.hashBlockFrom.ToString(), blockFrom.GetHash().ToString());
            BOOST_CHECK_EQUAL(from_utxo.nTimeBlockFrom, blockFrom.nTime);
            BOOST_CHECK_EQUAL(from_utxo.nTimeTxPrev, txPrev->nTime);
            BOOST_CHECK_EQUAL(from_utxo.nValueIn, txPrev->vout[n].nValue);

            ++checked;
        }
    }

    // A pass over an empty set would assert nothing.
    BOOST_CHECK(checked > 0);
}

//! A transaction from before Reddcoin carried a timestamp deserializes with
//! nTime zero, and the kernel falls back to the time of the block holding it.
//! Those coins are on mainnet, below the proof-of-stake switch, and no chain
//! this harness can build contains one, so drive the fallback directly: the
//! same coin described both ways, with the timestamp zeroed on each side.
BOOST_AUTO_TEST_CASE(zero_transaction_timestamp_falls_back_alike)
{
    LOCK(cs_main);
    CChainState& chainstate = m_node.chainman->ActiveChainstate();
    CCoinsViewCache& view = chainstate.CoinsTip();
    CBlockIndex* tip = chainstate.m_chain.Tip();
    BOOST_REQUIRE(tip);

    const COutPoint outpoint{m_coinbase_txns.front()->GetHash(), 0};
    BOOST_REQUIRE(!view.AccessCoin(outpoint).IsSpent());

    StakeKernelSource from_utxo;
    BOOST_REQUIRE(GetStakeKernelSource(&chainstate, view, outpoint, from_utxo));

    CTransactionRef txPrev;
    CBlockHeader blockFrom;
    unsigned int nTxOffset = 0;
    BOOST_REQUIRE(GetTransaction(&chainstate, outpoint, txPrev, blockFrom, nTxOffset));
    BOOST_REQUIRE(txPrev->nTime != 0); // the fixture stamps its transactions

    // Zero it on both sides, as a legacy coin would arrive.
    from_utxo.nTimeTxPrev = 0;
    CMutableTransaction legacy{*txPrev};
    legacy.nTime = 0;
    const CTransactionRef txLegacy = MakeTransactionRef(std::move(legacy));

    const unsigned int nBits = tip->nBits;
    const std::array<int64_t, 3> offsets{0, Params().GetConsensus().nStakeMinAge, Params().GetConsensus().nStakeMinAge + 7200};
    for (const int64_t offset : offsets) {
        const unsigned int nTimeTx = (unsigned int)(blockFrom.nTime + offset);
        uint256 proof_utxo;
        uint256 proof_disk;
        const bool ok_utxo = CheckStakeKernelHash(&chainstate, tip, nBits, from_utxo, outpoint.n, outpoint, nTimeTx, proof_utxo);
        const bool ok_disk = CheckStakeKernelHash(&chainstate, tip, nBits, blockFrom, outpoint.n, txLegacy, outpoint, nTimeTx, proof_disk);
        BOOST_CHECK_EQUAL(ok_utxo, ok_disk);
        BOOST_CHECK_EQUAL(proof_utxo.ToString(), proof_disk.ToString());
    }
}

//! The two overloads of CheckStakeKernelHash must hash the same preimage. Run
//! both over a coin at a range of timestamps and require that they agree on
//! the proof and on the verdict, including where the kernel is rejected for
//! being too young or for a timestamp violation.
BOOST_AUTO_TEST_CASE(both_overloads_hash_the_same_preimage)
{
    LOCK(cs_main);
    CChainState& chainstate = m_node.chainman->ActiveChainstate();
    CCoinsViewCache& view = chainstate.CoinsTip();
    CBlockIndex* tip = chainstate.m_chain.Tip();
    BOOST_REQUIRE(tip);
    const unsigned int nBits = tip->nBits;

    size_t compared = 0;
    for (const CTransactionRef& tx : m_coinbase_txns) {
        const COutPoint outpoint{tx->GetHash(), 0};
        if (view.AccessCoin(outpoint).IsSpent()) continue;

        StakeKernelSource from_utxo;
        BOOST_REQUIRE(GetStakeKernelSource(&chainstate, view, outpoint, from_utxo));

        CTransactionRef txPrev;
        CBlockHeader blockFrom;
        unsigned int nTxOffset = 0;
        BOOST_REQUIRE(GetTransaction(&chainstate, outpoint, txPrev, blockFrom, nTxOffset));

        // Below the minimum age, at it, and well past it, so the rejection
        // paths are compared as well as the hashing one.
        const int64_t nStakeMinAge = Params().GetConsensus().nStakeMinAge;
        const std::array<int64_t, 4> offsets{-1000, 0, nStakeMinAge, nStakeMinAge + 7200};
        for (const int64_t offset : offsets) {
            const unsigned int nTimeTx = (unsigned int)(blockFrom.nTime + offset);

            uint256 proof_utxo;
            uint256 proof_disk;
            const bool ok_utxo = CheckStakeKernelHash(&chainstate, tip, nBits, from_utxo, outpoint.n, outpoint, nTimeTx, proof_utxo);
            const bool ok_disk = CheckStakeKernelHash(&chainstate, tip, nBits, blockFrom, outpoint.n, txPrev, outpoint, nTimeTx, proof_disk);

            BOOST_CHECK_EQUAL(ok_utxo, ok_disk);
            BOOST_CHECK_EQUAL(proof_utxo.ToString(), proof_disk.ToString());
            ++compared;
        }
    }
    BOOST_CHECK(compared > 0);
}

BOOST_AUTO_TEST_SUITE_END()
