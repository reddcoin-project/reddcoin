// Copyright (c) 2014-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <net.h>
#include <signet.h>
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
BOOST_AUTO_TEST_CASE(signet_parse_tests)
{
    ArgsManager signet_argsman;
    signet_argsman.ForceSetArg("-signetchallenge", "51"); // set challenge to OP_TRUE
    const auto signet_params = CreateChainParams(signet_argsman, CBaseChainParams::SIGNET);
    CBlock block;
    BOOST_CHECK(signet_params->GetConsensus().signet_challenge == std::vector<uint8_t>{OP_TRUE});
    CScript challenge{OP_TRUE};

    // empty block is invalid
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // no witness commitment
    CMutableTransaction cb;
    cb.vout.emplace_back(0, CScript{});
    block.vtx.push_back(MakeTransactionRef(cb));
    block.vtx.push_back(MakeTransactionRef(cb)); // Add dummy tx to exercise merkle root code
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // no header is treated valid
    std::vector<uint8_t> witness_commitment_section_141{0xaa, 0x21, 0xa9, 0xed};
    for (int i = 0; i < 32; ++i) {
        witness_commitment_section_141.push_back(0xff);
    }
    cb.vout.at(0).scriptPubKey = CScript{} << OP_RETURN << witness_commitment_section_141;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(SignetTxs::Create(block, challenge));
    BOOST_CHECK(CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // no data after header, valid
    std::vector<uint8_t> witness_commitment_section_325{0xec, 0xc7, 0xda, 0xa2};
    cb.vout.at(0).scriptPubKey = CScript{} << OP_RETURN << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(SignetTxs::Create(block, challenge));
    BOOST_CHECK(CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // Premature end of data, invalid
    witness_commitment_section_325.push_back(0x01);
    witness_commitment_section_325.push_back(0x51);
    cb.vout.at(0).scriptPubKey = CScript{} << OP_RETURN << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // has data, valid
    witness_commitment_section_325.push_back(0x00);
    cb.vout.at(0).scriptPubKey = CScript{} << OP_RETURN << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(SignetTxs::Create(block, challenge));
    BOOST_CHECK(CheckSignetBlockSolution(block, signet_params->GetConsensus()));

    // Extraneous data, invalid
    witness_commitment_section_325.push_back(0x00);
    cb.vout.at(0).scriptPubKey = CScript{} << OP_RETURN << witness_commitment_section_141 << witness_commitment_section_325;
    block.vtx.at(0) = MakeTransactionRef(cb);
    BOOST_CHECK(!SignetTxs::Create(block, challenge));
    BOOST_CHECK(!CheckSignetBlockSolution(block, signet_params->GetConsensus()));
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
    BOOST_CHECK_EQUAL(out110.hash_serialized.ToString(), "1ebbf5850204c0bdb15bf030f47c7fe91d45c44c712697e4509ba67adb01c618");
    BOOST_CHECK_EQUAL(out110.nChainTx, 110U);

    const auto out210 = *ExpectedAssumeutxo(200, *params);
    BOOST_CHECK_EQUAL(out210.hash_serialized.ToString(), "51c8d11d8b5c1de51543c579736e786aa2736206d1e11e627568029ce092cf62");
    BOOST_CHECK_EQUAL(out210.nChainTx, 200U);
}

BOOST_AUTO_TEST_SUITE_END()
