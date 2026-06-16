// Copyright (c) 2014-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <net.h>
#include <uint256.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(validation_tests, TestingSetup)

// Test Reddcoin's unique block reward schedule
BOOST_AUTO_TEST_CASE(block_subsidy_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const Consensus::Params& consensusParams = chainParams->GetConsensus();

    // Genesis block (height 0): 10,000 RDD
    BOOST_CHECK_EQUAL(GetBlockSubsidy(0, consensusParams), 10000 * COIN);

    // Premine blocks (heights 1-10): 545,000,000 RDD each
    for (int height = 1; height <= 10; height++) {
        BOOST_CHECK_EQUAL(GetBlockSubsidy(height, consensusParams), 545000000 * COIN);
    }

    // Bonus period 1 (heights 11-9,999): 300,000 RDD
    BOOST_CHECK_EQUAL(GetBlockSubsidy(11, consensusParams), 300000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(5000, consensusParams), 300000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(9999, consensusParams), 300000 * COIN);

    // Bonus period 2 (heights 10,000-19,999): 200,000 RDD
    BOOST_CHECK_EQUAL(GetBlockSubsidy(10000, consensusParams), 200000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(15000, consensusParams), 200000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(19999, consensusParams), 200000 * COIN);

    // Bonus period 3 (heights 20,000-29,999): 150,000 RDD
    BOOST_CHECK_EQUAL(GetBlockSubsidy(20000, consensusParams), 150000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(25000, consensusParams), 150000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(29999, consensusParams), 150000 * COIN);

    // Standard reward period (heights 30,000-139,999): 100,000 RDD
    BOOST_CHECK_EQUAL(GetBlockSubsidy(30000, consensusParams), 100000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(100000, consensusParams), 100000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(139999, consensusParams), 100000 * COIN);

    // Halving period starts at 140,000
    // First halving at 140,000: 100,000 >> 1 = 50,000 RDD
    BOOST_CHECK_EQUAL(GetBlockSubsidy(140000, consensusParams), 50000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(189999, consensusParams), 50000 * COIN);

    // Second halving at 190,000: 50,000 >> 1 = 25,000 RDD
    BOOST_CHECK_EQUAL(GetBlockSubsidy(190000, consensusParams), 25000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(239999, consensusParams), 25000 * COIN);

    // Third halving at 240,000: 25,000 >> 1 = 12,500 RDD
    BOOST_CHECK_EQUAL(GetBlockSubsidy(240000, consensusParams), 12500 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(289999, consensusParams), 12500 * COIN);

    // Fourth halving at 290,000: 12,500 >> 1 = 6,250 RDD
    BOOST_CHECK_EQUAL(GetBlockSubsidy(290000, consensusParams), 6250 * COIN);

    // Test many halvings into the future
    BOOST_CHECK_EQUAL(GetBlockSubsidy(340000, consensusParams), 3125 * COIN);
    BOOST_CHECK_EQUAL(GetBlockSubsidy(390000, consensusParams), 1562 * COIN + 50000000); // 1562.5 RDD
    BOOST_CHECK_EQUAL(GetBlockSubsidy(440000, consensusParams), 781 * COIN + 25000000); // 781.25 RDD
}

// Test Reddcoin's total PoW supply calculation
BOOST_AUTO_TEST_CASE(subsidy_limit_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const Consensus::Params& consensusParams = chainParams->GetConsensus();

    CAmount nSum = 0;

    // Genesis block (height 0)
    nSum += GetBlockSubsidy(0, consensusParams);
    BOOST_CHECK_EQUAL(nSum, 10000 * COIN);

    // Premine (heights 1-10): 10 blocks × 545,000,000 RDD
    for (int h = 1; h <= 10; h++) {
        nSum += GetBlockSubsidy(h, consensusParams);
    }
    BOOST_CHECK_EQUAL(nSum, 10000 * COIN + 5450000000LL * COIN);

    // Bonus period 1 (heights 11-9,999): 9,989 blocks × 300,000 RDD
    for (int h = 11; h < 10000; h++) {
        nSum += GetBlockSubsidy(h, consensusParams);
    }
    CAmount expectedAfterBonus1 = 10000LL * COIN + 5450000000LL * COIN + 9989LL * 300000LL * COIN;
    BOOST_CHECK_EQUAL(nSum, expectedAfterBonus1);

    // Bonus period 2 (heights 10,000-19,999): 10,000 blocks × 200,000 RDD
    for (int h = 10000; h < 20000; h++) {
        nSum += GetBlockSubsidy(h, consensusParams);
    }
    CAmount expectedAfterBonus2 = expectedAfterBonus1 + 10000LL * 200000LL * COIN;
    BOOST_CHECK_EQUAL(nSum, expectedAfterBonus2);

    // Bonus period 3 (heights 20,000-29,999): 10,000 blocks × 150,000 RDD
    for (int h = 20000; h < 30000; h++) {
        nSum += GetBlockSubsidy(h, consensusParams);
    }
    CAmount expectedAfterBonus3 = expectedAfterBonus2 + 10000LL * 150000LL * COIN;
    BOOST_CHECK_EQUAL(nSum, expectedAfterBonus3);

    // Standard period (heights 30,000-139,999): 110,000 blocks × 100,000 RDD
    for (int h = 30000; h < 140000; h++) {
        nSum += GetBlockSubsidy(h, consensusParams);
    }
    CAmount expectedAfterStandard = expectedAfterBonus3 + 110000LL * 100000LL * COIN;
    BOOST_CHECK_EQUAL(nSum, expectedAfterStandard);

    // First halving period (heights 140,000-189,999): 50,000 blocks × 50,000 RDD
    for (int h = 140000; h < 190000; h++) {
        nSum += GetBlockSubsidy(h, consensusParams);
    }
    CAmount expectedAfterHalving1 = expectedAfterStandard + 50000LL * 50000LL * COIN;
    BOOST_CHECK_EQUAL(nSum, expectedAfterHalving1);

    // Verify all money is in valid range
    BOOST_CHECK(MoneyRange(nSum));

    // Test that subsidy becomes small after many halvings
    // Note: Right shift operations in C++ are limited to the bit width (63 for int64_t)
    // so extremely high blocks will cycle rather than reach 0
    CAmount farFuture = GetBlockSubsidy(1000000, consensusParams);
    BOOST_CHECK(farFuture < 100 * COIN); // Less than 100 RDD after ~17 halvings
}

// Test Proof-of-Stake reward calculation
BOOST_AUTO_TEST_CASE(pos_reward_test)
{
    // PoS reward is based on coin age: (coin-days × annual rate) / 365
    // Annual rate is 5% (COIN_YEAR_REWARD = 5 * CENT = 5000000)

    // Test 1: 1 coin for 365 days should give ~5% annual return
    int64_t nCoinAge = 365; // 1 coin × 365 days
    CAmount nFees = 0;
    CAmount nReward = GetProofOfStakeReward(nCoinAge, nFees);

    // Formula: nCoinAge * COIN_YEAR_REWARD * 33 / (365 * 33 + 8)
    // Expected: 365 * 5000000 * 33 / 12053 ≈ 4,996,681
    CAmount expected = 4996681; // ~0.04996681 RDD for 365 coin-days
    BOOST_CHECK_EQUAL(nReward, expected);

    // Test 2: 10 coins for 100 days = 1000 coin-days
    nCoinAge = 1000;
    nReward = GetProofOfStakeReward(nCoinAge, nFees);
    expected = 13689537; // ~0.13689537 RDD
    BOOST_CHECK_EQUAL(nReward, expected);

    // Test 3: With fees
    nCoinAge = 365;
    nFees = 1000 * COIN;
    nReward = GetProofOfStakeReward(nCoinAge, nFees);
    expected = 4996681 + 1000 * COIN; // Base reward + fees
    BOOST_CHECK_EQUAL(nReward, expected);

    // Test 4: Zero coin age
    nCoinAge = 0;
    nFees = 0;
    nReward = GetProofOfStakeReward(nCoinAge, nFees);
    BOOST_CHECK_EQUAL(nReward, 0);
}

//! Test retrieval of valid assumeutxo values.
BOOST_AUTO_TEST_CASE(test_assumeutxo)
{
    const auto params = CreateChainParams(*m_node.args, CBaseChainParams::REGTEST);

    // These heights don't have assumeutxo configurations associated, per the contents
    // of chainparams.cpp.
    std::vector<int> bad_heights{0, 100, 111, 115, 209, 211};

    for (auto empty : bad_heights) {
        const auto out = ExpectedAssumeutxo(empty, *params);
        BOOST_CHECK(!out);
    }

    const auto out110 = *ExpectedAssumeutxo(110, *params);
    // Reddcoin: UTXO set hash and nChainTx for the regtest PoS test chain,
    // computed via gettxoutsetinfo (HASH_SERIALIZED) on the chain built by the
    // C++ staking helpers (genesis + 89 PoW + PoS blocks).
    BOOST_CHECK_EQUAL(out110.hash_serialized.ToString(), "106f9f9bd6c57cea84444957b5630e4a1679a658c73455a0e0362ff0174ee181");
    BOOST_CHECK_EQUAL(out110.nChainTx, 132U);  // genesis + 89 PoW + 21 PoS (2 tx each)

    const auto out210 = *ExpectedAssumeutxo(200, *params);
    BOOST_CHECK_EQUAL(out210.hash_serialized.ToString(), "8938769375b98ee91f968cbe547cd364d7977317cc3d5241c4dca8fc92912569");
    BOOST_CHECK_EQUAL(out210.nChainTx, 312U);  // genesis + 89 PoW + 111 PoS (2 tx each)
}

BOOST_AUTO_TEST_SUITE_END()
